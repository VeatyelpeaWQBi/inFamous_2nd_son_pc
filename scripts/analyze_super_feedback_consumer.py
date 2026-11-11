"""Read-only bounded disassembly for the newly grouped worker read site.

Uses installed LLVM and the existing SELF reader; writes derived doc files only.
No game extraction, source patch, build, live memory access or runtime test.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess

from analyze_game import elf_for_window, llvm_path
from game_image import Image, read_identity
from project_paths import ROOT


def main():
    project = json.loads((ROOT / "project.json").read_text(encoding="utf-8"))
    directory = ROOT / "game/CUSA00309"
    identity = read_identity(directory / "sce_sys/param.sfo")
    image = Image(directory / "eboot.bin")
    digest = hashlib.sha256(image.data).hexdigest()
    if (digest != project["game_sha256"] or identity["TITLE_ID"] != project["title"]
            or identity["APP_VER"] != project["version"]):
        raise ValueError("Pinned game identity mismatch; no known-address interpretation")
    tool = llvm_path(None)
    if tool is None:
        raise FileNotFoundError("Existing system LLVM is required; no substitute installed")
    population = json.loads((ROOT / "doc/super-feedback-population-20261102.json").read_text())
    captured_main = int("6fc3eff", 16)
    known_main_rva = int("3f3eff", 16)
    base = captured_main - known_main_rva
    selected = [g for g in population["phases"]["peak"]["groups"]
                if int(g["site"], 16) == base + 0x3ff6d0]
    if not selected:
        raise ValueError("No observed worker requests for this bounded window")
    start, length, site = 0x3fed00, 0xe00, 0x3ff6d0
    raw = image.read(start, length)
    run = subprocess.run([tool, "-d", "--x86-asm-syntax=intel", "-"],
                         input=elf_for_window(start, raw), capture_output=True, check=True)
    assembly = run.stdout.decode("utf-8", errors="replace")
    if not re.search(rf"^\s*{site:x}:\s", assembly, re.MULTILINE):
        raise ValueError("Captured worker PC is not an instruction boundary in the pinned game")
    out = ROOT / "doc/super-core-20261031/game/particle-postprocess-worker-read.asm.txt"
    out.write_text(assembly, encoding="utf-8")
    report = dict(schema=1, offline_only=True, source_game_written=False,
                  game_sha256=digest, identity=identity, llvm=tool,
                  captured_site=hex(base + site), derived_load_base=hex(base), rva=hex(site),
                  load_base_derivation="captured main read site minus pinned main MOV RVA; "
                                       "worker opcode additionally checked in bounded disassembly",
                  input_window=dict(rva=hex(start), size=length,
                                    sha256=hashlib.sha256(raw).hexdigest()),
                  disassembly=str(out.relative_to(ROOT)),
                  peak_requests=sum(g["requests"] for g in selected),
                  peak_distinct_addresses=len(selected),
                  scope="bounded window, not a recovered complete function or dynamic register trace")
    target = ROOT / "doc/super-feedback-worker-consumer-20261102.json"
    target.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    lines = assembly.splitlines()
    at = next(i for i, line in enumerate(lines) if re.match(rf"\s*{site:x}:", line))
    print("\n".join(lines[max(0, at-14):at+22]))
    print("Saved bounded consumer disassembly:", out.relative_to(ROOT))


if __name__ == "__main__":
    main()
