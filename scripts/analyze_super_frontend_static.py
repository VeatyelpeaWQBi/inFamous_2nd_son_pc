"""Offline symbol/disassembly audit of existing captures; never attaches to a process.

Uses the existing system LLVM tools. Writes a small derived JSON in doc only.
Does not build, run tests, start the game, change tuning or read live process memory.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
LLVM = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\Llvm\x64\bin")
OLD = "cdce4768e081c0ab30058de4575208a0c50c5952f27c054bb3bd93a0bd4a3115"
ARCHIVE = ROOT / "state/analysis-builds" / OLD
EXE = ARCHIVE / "inFAMOUS Second Son.exe"
DLL = Path(r"C:\Windows\System32\vcruntime140.dll")
LOG = ROOT / "state/super-captures/20261030-194642-pro4k-e697ae2b/launcher-output.log"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(data)
    return digest.hexdigest()


def invoke(tool, *args):
    executable = LLVM / tool
    if not executable.is_file():
        raise FileNotFoundError(f"Existing system tool unavailable: {executable}")
    return subprocess.run([str(executable), *map(str, args)], check=True,
                          capture_output=True, text=True, encoding="utf-8").stdout


def main():
    actual_hash = sha256(EXE)
    if actual_hash != OLD:
        raise ValueError("Archive does not match the captured executable; no report written.")
    # Each PC is a sampled function-relative hot offset from the retained native log.
    pcs = [0x7d55c6, 0x7d55da, 0x7cc42f, 0x7cc215, 0x7cbffb,
           0x746173, 0x74616c, 0x746249, 0x816a5a, 0x8179ec,
           0x818044, 0x766e84, 0x77595c, 0x77c8d4]
    resolved = json.loads(invoke("llvm-symbolizer.exe", f"--obj={EXE}",
                                "--relative-address", "--output-style=JSON",
                                *(hex(pc) for pc in pcs)))
    hot_evidence = []
    observed_pcs = set()
    for number, line in enumerate(LOG.read_text(encoding="utf-8-sig").splitlines(), 1):
        match = re.search(r"Perf hot \[shadPS4:GpuCommandProcessor\]\s+([\d.]+)% "
                          r"inFAMOUS Second Son.exe\+0x([0-9a-f]+):(.*)", line)
        if not match:
            continue
        function = int(match[2], 16)
        for offset, percent in re.findall(r"\+0x([0-9a-f]+) ([\d.]+)%", match[3]):
            pc = function + int(offset, 16)
            if pc in pcs:
                observed_pcs.add(pc)
                hot_evidence.append(dict(log_line=number, pc=hex(pc),
                                         function_percent=float(match[1]),
                                         offset_percent_within_function=float(percent)))
    if observed_pcs != set(pcs):
        raise ValueError("Selected instruction is absent from the recorded hot offsets: "
                         + ", ".join(hex(pc) for pc in sorted(set(pcs) - observed_pcs)))
    private_headers = invoke("llvm-objdump.exe", "--private-headers", DLL)
    image_base = int(re.search(r"ImageBase\s+([0-9a-fA-F]+)", private_headers)[1], 16)
    exports = [line.strip() for line in private_headers.splitlines()
               if re.search(r"\b(memcmp|memcpy|memmove|memset)$", line)]
    regions = {}
    for label, begin, end in [("export_thunks", 0x1f010, 0x1f045),
                              ("internal_copy_entry", 0x1cb40, 0x1cbc0),
                              ("memmove_entry", 0x1db00, 0x1db90)]:
        regions[label] = invoke("llvm-objdump.exe", "--disassemble",
                                f"--start-address={hex(image_base + begin)}",
                                f"--stop-address={hex(image_base + end)}", DLL)
    # Current DLL identity is explicit: the historical capture did not archive this DLL.
    # Export jumps prove the meaning of these RVAs in this file, not historical identity.
    current_log = ROOT / "state/super-captures/20261102-215526-pro4k-43e5c1fc/launcher-output.log"
    tuning_line = next(line for line in current_log.read_text(encoding="utf-8-sig").splitlines()
                       if "Perf tuning applied:" in line)
    evidence = {
        "schema": 1, "mode": "offline-static-only", "game_or_test_started": False,
        "runtime_sha256": actual_hash, "archive": str(ARCHIVE.relative_to(ROOT)),
        "native_log": str(LOG.relative_to(ROOT)), "resolved_sample_pcs": resolved,
        "recorded_hot_offsets": hot_evidence,
        "audited_current_sources": {name: sha256(ROOT / name) for name in (
            "src/video_core/amdgpu/scout.cpp", "src/video_core/amdgpu/scout.h",
            "src/video_core/renderer_vulkan/vk_pipeline_cache.cpp",
            "src/video_core/renderer_vulkan/vk_pipeline_cache.h",
            "src/video_core/renderer_vulkan/vk_rasterizer.cpp",
            "src/video_core/renderer_vulkan/vk_rasterizer.h")},
        "current_dll": {"path": str(DLL), "sha256": sha256(DLL),
                        "image_base": hex(image_base), "exports": exports,
                        "disassembly": regions, "historical_identity_verified": False},
        "current_baseline_tuning_log": str(current_log.relative_to(ROOT)),
        "current_baseline_tuning_receipt": tuning_line,
        "limits": [
            "Old profile windows span 20 seconds; sampled percentages are not peak-exclusive.",
            "PDB line numbers belong to the archived build, not current source line offsets.",
            "Current baseline has runtime_lookup_memo enabled; old hot work may already differ.",
            "A hot instruction or inclusive stack does not establish removable frame cost.",
            "Current DLL export targets do not prove historical DLL bytes or version.",
            "Scout audit concerns a disabled path; no evidence it caused current FPS loss.",
        ],
    }
    target = ROOT / "doc/super-frontend-static-audit-20261102.json"
    target.write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Offline audit saved: {target.relative_to(ROOT)} ({target.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
