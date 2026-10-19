"""Read-only, bounded binary analysis; existing LLVM is optional. No game extraction on disk."""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
from pathlib import Path
from game_image import Image, read_identity
from project_paths import ROOT, resolve_project_path, project_child, relative_path, write_text, write_json
WINDOWS = [
    ("exposure-allocation", 0x172320, 160),
    ("exposure-consumer", 0x171680, 768),
    ("particle-manager", 0x3FE890, 384),
    ("emitter-prepare", 0x3EFDE0, 640),
    ("feedback-consumer", 0x3F3ED0, 512),
    ("feedback-list", 0x3EDEF0, 640),
    ("particle-postprocess", 0x3FED00, 512),
    ("particle-resource-bindings", 0x3FAB70, 640),
    ("particle-sort-stages", 0x3F1A60, 640),
    ("emitter-program-selection", 0x3F2BD0, 512),
    ("auxiliary-range-reservation", 0x16DD50, 512),
    ("particle-submit", 0x1691E0, 640),
    ("compute-stage-sync", 0x1694E0, 256),
    ("compute-completion-poll", 0x1695E0, 256),
    ("busy-wait-helper", 0x53B2B0, 160),
    ("light-list-allocation", 0x16BDF0, 96),
    ("resolution-setup", 0x1EFF10, 128),
    ("pro-resolution-setup", 0x1F0C80, 256),
]
def elf_for_window(address, raw):
    names = b"\0.text\0.shstrtab\0"
    names_at = 64 + len(raw)
    table_at = (names_at + len(names) + 7) & ~7
    header = struct.pack("<16sHHIQQQIHHHHHH", b"\x7fELF\x02\x01\x01" + bytes(9),
                         2, 62, 1, address, 0, table_at, 0, 64, 0, 0, 64, 3, 2)
    null = bytes(64)
    text = struct.pack("<IIQQQQIIQQ", 1, 1, 6, address, 64, len(raw), 0, 0, 16, 0)
    strings = struct.pack("<IIQQQQIIQQ", 7, 3, 0, 0, names_at, len(names), 0, 0, 1, 0)
    result = header + raw + names
    return result + bytes(table_at - len(result)) + null + text + strings

def llvm_path(explicit):
    if explicit:
        path = resolve_project_path(explicit)
        if not path.is_file():
            raise ValueError("Configured LLVM tool does not exist")
        return str(path)
    found = shutil.which("llvm-objdump.exe")
    if found:
        return found
    if os.name == "nt":
        import winreg
        for hive, key in ((winreg.HKEY_CURRENT_USER, "Environment"),
                          (winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment")):
            try:
                with winreg.OpenKey(hive, key) as handle:
                    value, _ = winreg.QueryValueEx(handle, "Path")
                for directory in os.path.expandvars(value).split(os.pathsep):
                    candidate = Path(directory.strip().strip('"')) / "llvm-objdump.exe"
                    if directory and candidate.is_file():
                        return str(candidate)
            except OSError:
                pass
        program_files = os.environ.get("ProgramFiles(x86)")
        if program_files:
            visual_studio = Path(program_files) / "Microsoft Visual Studio"
            for candidate in sorted(visual_studio.glob("*/*/VC/Tools/Llvm/x64/bin/llvm-objdump.exe"), reverse=True):
                if candidate.is_file():
                    return str(candidate)
    return None

def analyze(output, llvm=None):
    game = project_child("game/CUSA00309")
    project = json.loads((ROOT / "project.json").read_text(encoding="utf-8"))
    image = Image(game / "eboot.bin")
    sha = hashlib.sha256(image.data).hexdigest()
    if sha != project["game_sha256"]:
        raise ValueError("Unexpected executable: refuse to interpret known addresses")
    identity = read_identity(game / "sce_sys/param.sfo")
    if not all((
        identity["TITLE_ID"] == project["title"],
        identity["APP_VER"] == project["version"],
    )):
        raise ValueError("Game identity does not match the pinned project")
    tool = llvm_path(llvm)
    output = project_child(relative_path(output))
    if not output.is_relative_to(project_child("doc")):
        raise ValueError("Analysis output must stay below doc")
    output.mkdir(parents=True, exist_ok=True)
    windows = []
    for name, address, size in WINDOWS:
        raw = image.read(address, size)
        item = {"name": name, "rva": hex(address), "size": size,
                "sha256": hashlib.sha256(raw).hexdigest(), "direct_calls": [],
                "scope": "bounded instruction window, not a recovered complete function"}
        if tool:
            run = subprocess.run([tool, "-d", "--x86-asm-syntax=intel", "-"],
                                 input=elf_for_window(address, raw),
                                 capture_output=True, check=True)
            assembly = run.stdout.decode("utf-8", errors="replace")
            write_text(output / (name + ".asm.txt"), assembly)
            for line in assembly.splitlines():
                if re.search(r"\bcall\b", line):
                    match = re.search(r"\bcall\s+0x([0-9a-f]+)", line)
                    if match:
                        item["direct_calls"].append(hex(int(match.group(1), 16)))
            item["disassembly"] = name + ".asm.txt"
        windows.append(item)
    checks = {
        "particle_feedback_load": image.read(0x3F3EFF, 3).hex() == "8b4004",
        "particle_count_consumed": image.read(0x3F3F09, 3).hex() == "894108",
        "exposure_hook_original": image.read(0x171704, 7).hex() == "48c1e7204809c7",
        "cpu_poll_uses_rdtsc": b"\x0f\x31" in image.read(0x53B2B0, 160),
    }
    symbols = []
    for token in (b"CParticleSystemInstance", b"psysnew.cpp", b"luminance", b"Luminance"):
        offsets, at = [], 0
        while len(offsets) < 16:
            at = image.data.find(token, at)
            if at < 0:
                break
            offsets.append(at)
            at += len(token)
        symbols.append({"token": token.decode("ascii"), "file_offsets": offsets})
    report = {
        "schema": 1, "identity": identity, "sha256": sha,
        "input_bytes": len(image.data), "segments": image.segments,
        "llvm": Path(tool).name if tool else None, "windows": windows, "checks": checks, "strings": symbols,
        "source_game_written": False, "runtime_measured": False,
        "function_recovery_complete": False,
    }
    write_json(output / "game-analysis.json", report)
    if not all(checks.values()):
        raise ValueError("One or more named instruction checks failed; review the report")
    return report

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path("doc/binary-analysis"))
    parser.add_argument("--llvm")
    args = parser.parse_args()
    result = analyze(project_child(args.output), args.llvm)
    print(json.dumps({"sha256": result["sha256"], "windows": len(result["windows"]),
                      "checks": result["checks"], "llvm": result["llvm"]}, indent=2))
