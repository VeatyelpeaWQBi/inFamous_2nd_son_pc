"""Prepare two concurrency prerequisites, without editing any engine/build input.

This is not a performance candidate. Does not enable Scout, fix all of its input
validation, apply the patch, build, test, start the game, or change running settings.
"""
import difflib
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    edits = {
        "src/video_core/renderer_vulkan/vk_pipeline_cache.cpp": [
            ("Shader::RuntimeInfo runtime_info = runtime_infos[0];",
             "// Private output: BuildRuntimeInfoFrom initializes every byte.\n"
             "        // Never copy the command processor's concurrently mutable runtime info.\n"
             "        Shader::RuntimeInfo runtime_info{};"),
        ],
        "src/video_core/renderer_vulkan/vk_pipeline_cache.h": [
            ("u32 memo_next{}; ///< Command processor only.",
             "std::atomic<u32> memo_next{}; ///< CP writes; scout reads only the search order."),
            ("auto& entry = memo[memo_next];",
             "const u32 next = memo_next.load(std::memory_order_relaxed);\n"
             "        auto& entry = memo[next];"),
            ("memo_next = (memo_next + 1) % NumMemoEntries;",
             "memo_next.store((next + 1) % NumMemoEntries, std::memory_order_relaxed);"),
            ("""        for (size_t n = 0; n < NumMemoEntries; ++n) {
            // memo_next is the command processor's: a stale value only changes the order.
            const auto& entry =
                memo[(NumMemoEntries + memo_next - 1 - n) % NumMemoEntries];""",
             """        // The cursor only orders the scan; the entry sequence protects its payload.
        const u32 next = memo_next.load(std::memory_order_relaxed);
        for (size_t n = 0; n < NumMemoEntries; ++n) {
            const auto& entry = memo[(NumMemoEntries + next - 1 - n) % NumMemoEntries];"""),
        ],
    }
    diff, files = [], []
    for name, replacements in edits.items():
        raw = (ROOT / name).read_bytes()
        before = raw.decode("utf-8").replace("\r\n", "\n")
        after = before
        for old, new in replacements:
            if after.count(old) != 1:
                raise ValueError("Source differs from reviewed location; stop: " + name)
            after = after.replace(old, new)
        diff.extend(difflib.unified_diff(before.splitlines(keepends=True),
                                         after.splitlines(keepends=True),
                                         fromfile="a/" + name, tofile="b/" + name))
        files.append(dict(path=name, original_sha256=hashlib.sha256(raw).hexdigest(),
                          prepared_lf_sha256=hashlib.sha256(after.encode()).hexdigest()))
    target = ROOT / "scripts/patches/scout-concurrency-prerequisites.patch"
    target.write_text("".join(diff), encoding="utf-8", newline="\n")
    manifest = dict(schema=1, status="prepared-not-applied", files=files,
                    patch=str(target.relative_to(ROOT)), game_or_test_started=False,
                    engine_source_changed=False, build_started=False,
                    scout_enabled=False, performance_candidate=False,
                    limitations=[
                        "Only removes the two identified unsynchronized host reads.",
                        "Does not establish descriptor-table freshness or full register equivalence.",
                        "Does not address shader hot-reload quiescence or command-buffer lifetime.",
                        "Must not enable Scout or claim FPS benefit from these prerequisites.",
                    ])
    (ROOT / "doc/scout-concurrency-prerequisites.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared only: {target.relative_to(ROOT)} ({target.stat().st_size} bytes). "
          "Engine, build and game untouched.")


if __name__ == "__main__":
    main()
