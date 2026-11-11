"""Prepare a reviewable patch only. Never edits build inputs, applies, builds or runs it."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
changes = {}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--apply', action='store_true', help='Apply the already-reviewed preparation; requires explicit user authorization.')
args = parser.parse_args()
old_manifest = json.loads((ROOT/'doc/compute-recorder-patch-manifest.json').read_text()) if args.apply else None


def replace(path, old, new):
    original = (ROOT/path).read_text(encoding='utf-8')
    current = changes.get(path, (original, original))[1]
    if current.count(old) != 1:
        raise ValueError(f'Expected one exact edit location in {path}: {old[:70]!r}')
    changes[path] = original, current.replace(old, new)


def candidates(path):
    original = (ROOT/path).read_text(encoding='utf-8')
    old = "'gds-counter','cpu-placement'"
    if old not in original:
        raise ValueError(f'Candidate list not found: {path}')
    changes[path] = original, original.replace(old, old+",'compute-recorder'")


replace('CMakeLists.txt',
        'add_executable(ifss_path_trace_queue_test EXCLUDE_FROM_ALL tests/test_path_trace_queue.cpp)',
        '''# Reuse the existing shared-readiness concurrency oracle; not part of the game build.
add_executable(ifss_compile_readiness_test EXCLUDE_FROM_ALL tests/cache_runtime/test_compile_readiness.cpp)
target_compile_features(ifss_compile_readiness_test PRIVATE cxx_std_23)
target_include_directories(ifss_compile_readiness_test PRIVATE src)

add_executable(ifss_path_trace_queue_test EXCLUDE_FROM_ALL tests/test_path_trace_queue.cpp)''')
replace('src/common/perf_stats.h', '    std::atomic<bool> asc_queue{false};',
        '''    std::atomic<bool> asc_queue{false};
    /// Experimental: allow the independent compute scheduler to use its own CPU recorder.
    /// Per-session mode still obeys threaded_recording; default and ordinary baseline stay off.
    std::atomic<bool> compute_threaded_recording{false};''')
replace('src/core/perf_sampler.cpp', '    t.gds_counter_scope.store(false, relaxed);',
        '    t.gds_counter_scope.store(false, relaxed);\n    t.compute_threaded_recording.store(false, relaxed);')
replace('src/core/perf_sampler.cpp', '        } else if (key == "asc_queue") {',
        '''        } else if (key == "compute_threaded_recording") {
            tuning.compute_threaded_recording.store(value != "0", std::memory_order_relaxed);
        } else if (key == "asc_queue") {''')
replace('src/core/perf_sampler.cpp',
        '''        return name == "shadPS4:GpuCommandProcessor" || name == "shadPS4:GpuCommandRecorder" ||
               name == "shadPS4:Scout" || name == "shadPS4:GpuUploadWorker";''',
        '''        return name == "shadPS4:GpuCommandProcessor" || name == "shadPS4:GpuCommandRecorder" ||
               name == "shadPS4:ComputeRecorder" || name == "shadPS4:Scout" ||
               name == "shadPS4:GpuUploadWorker";''')
replace('src/core/benchmark_control.h', 'void NoteNativeCompute(bool graphics);',
        '''void NoteNativeCompute(bool graphics);
void NoteComputeRecorderSubmit();
// Reuses the existing sampler's GetThreadTimes deltas; 100% means one logical processor.
void NoteThreadCpu(std::string_view name, double cpu, double kernel, double interval_seconds);''')
replace('src/core/benchmark_control.cpp', '#include <locale>', '#include <locale>\n#include <mutex>')
replace('src/core/benchmark_control.cpp', 'std::atomic<u64> native_compute{}, native_graphics_compute{};',
        'std::atomic<u64> native_compute{}, native_graphics_compute{};\nstd::atomic<u64> compute_recorder_submits{};')
replace('src/core/benchmark_control.cpp', 'void NoteGdsCounterScope(bool reduced_conflict) {',
        '''void NoteComputeRecorderSubmit() {
    if (Enabled()) compute_recorder_submits.fetch_add(1, std::memory_order_relaxed);
}

void NoteGdsCounterScope(bool reduced_conflict) {''')
replace('src/core/benchmark_control.cpp', 'Libraries::Pad::OrbisPadButtonDataOffset held_button{};',
        '''struct ThreadCpuSample {
    double cpu{}, kernel{}, seconds{};
    u64 observed_ns{};
};
constexpr std::array ThreadCpuNames{
    "shadPS4:GpuCommandProcessor", "shadPS4:GpuCommandRecorder", "shadPS4:ComputeRecorder",
    "shadPS4:GpuUploadWorker", "Game:Main"};
constexpr std::array ThreadCpuKeys{
    "command_processor", "graphics_recorder", "compute_recorder", "upload_worker", "game_main"};
std::array<ThreadCpuSample, ThreadCpuNames.size()> thread_cpu_samples{};
std::mutex thread_cpu_mutex;
nlohmann::json ThreadCpuStatus() {
    nlohmann::json result{{"units", "percent_of_one_logical_processor"}};
    std::scoped_lock lock{thread_cpu_mutex};
    const u64 now = Common::Perf::NowNs();
    for (size_t i = 0; i < thread_cpu_samples.size(); ++i) {
        const auto& s = thread_cpu_samples[i];
        const bool valid = s.observed_ns != 0 && now >= s.observed_ns &&
            now - s.observed_ns <= 3'000'000'000 && s.seconds > 0.0 && s.seconds < 5.0 &&
            s.cpu >= 0.0 && s.cpu <= 200.0 && s.kernel >= 0.0 && s.kernel <= s.cpu;
        result[ThreadCpuKeys[i]] = {{"available", valid}};
        if (valid) {
            result[ThreadCpuKeys[i]].update({{"cpu_percent", s.cpu}, {"kernel_percent", s.kernel},
                {"window_seconds", s.seconds}, {"sample_age_ms", static_cast<double>(now - s.observed_ns) / 1e6}});
        }
    }
    return result;
}
Libraries::Pad::OrbisPadButtonDataOffset held_button{};''')
replace('src/core/benchmark_control.cpp', 'void NoteComputeRecorderSubmit() {',
        '''void NoteThreadCpu(std::string_view name, double cpu, double kernel, double seconds) {
    if (!Enabled()) return;
    for (size_t i = 0; i < ThreadCpuNames.size(); ++i) {
        if (name == ThreadCpuNames[i]) {
            std::scoped_lock lock{thread_cpu_mutex};
            thread_cpu_samples[i] = {cpu, kernel, seconds, Common::Perf::NowNs()};
            return;
        }
    }
}

void NoteComputeRecorderSubmit() {''')
replace('src/core/benchmark_control.cpp',
        '            if (tuning.gpu_threads_affinity.load() == Common::Perf::AffinityAuto) {',
        '''            status["thread_cpu"] = ThreadCpuStatus();
            if (tuning.gpu_threads_affinity.load() == Common::Perf::AffinityAuto) {''')
replace('src/core/perf_sampler.cpp', '            if (info.has_baseline && interval_s > 0.0) {',
        '''            const bool has_cpu_sample = info.has_baseline && interval_s > 0.0;
            if (has_cpu_sample) {''')
replace('src/core/perf_sampler.cpp', '            by_cpu.emplace_back(info.cpu, tid);',
        '''            by_cpu.emplace_back(info.cpu, tid);
            if (has_cpu_sample) {
                Core::Benchmark::NoteThreadCpu(info.name, info.cpu, info.kernel, interval_s);
            }''')
replace('src/core/benchmark_control.cpp', '                {"asc_queue", tuning.asc_queue.load()},',
        '''                {"asc_queue", tuning.asc_queue.load()},
                {"threaded_recording", tuning.threaded_recording.load()},
                {"compute_threaded_recording", tuning.compute_threaded_recording.load()},
                {"compute_recorder_submits", compute_recorder_submits.load(std::memory_order_relaxed)},''')
replace('src/video_core/renderer_vulkan/vk_scheduler.cpp', '#include "common/thread.h"',
        '#include "common/thread.h"\n#include "core/benchmark_control.h"')
replace('src/video_core/renderer_vulkan/vk_scheduler.cpp',
        '    Common::SetCurrentThreadName("shadPS4:GpuCommandRecorder");',
        '''    Common::SetCurrentThreadName(std::strcmp(name, "compute") == 0
                                     ? "shadPS4:ComputeRecorder"
                                     : "shadPS4:GpuCommandRecorder");''')
replace('src/video_core/renderer_vulkan/vk_scheduler.cpp',
        '            SubmitToQueue(std::span{&cmdbuf, 1}, job.info);',
        '''            SubmitToQueue(std::span{&cmdbuf, 1}, job.info);
            if (std::strcmp(name, "compute") == 0) {
                // Actual worker submission, not a requested flag or a CP dispatch count.
                Core::Benchmark::NoteComputeRecorderSubmit();
            }''')
replace('src/video_core/renderer_vulkan/vk_scheduler.cpp',
        '''    stream_session = threading_allowed &&
                     Common::Perf::GetTuning().threaded_recording.load(std::memory_order_relaxed);''',
        '''    const auto& tuning = Common::Perf::GetTuning();
    const bool compute_recording = std::strcmp(name, "compute") != 0 ||
        tuning.compute_threaded_recording.load(std::memory_order_relaxed);
    stream_session = threading_allowed && compute_recording &&
                     tuning.threaded_recording.load(std::memory_order_relaxed);''')
replace('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
        '''        LOG_INFO(Render_Vulkan, "asc_queue: compute work on a queue of its own");
    }
    return true;''',
        '''        LOG_INFO(Render_Vulkan, "asc_queue: compute work on a queue of its own");
    }
    if (Common::Perf::GetTuning().compute_threaded_recording.load(std::memory_order_relaxed)) {
        // The first session may remain direct; later sessions use immutable command streams.
        // BeginSession also checks the flag, so disabling it drains the recorder before direct use.
        compute_scheduler->AllowThreadedRecording();
    }
    return true;''')

for path in ('scripts/collect_super.ps1', 'scripts/collect_feedback_pair.ps1',
             'scripts/super_capture_tools.ps1', 'tests/bootstrap/test_feedback_variants.ps1'):
    candidates(path)
replace('scripts/super_capture_tools.ps1',
        'readback_feedback_checkpoint=0;readback_feedback_writers=0;readback_word_load=0;asc_queue=0;gds_counter_scope=0}',
        'readback_feedback_checkpoint=0;readback_feedback_writers=0;readback_word_load=0;asc_queue=0;gds_counter_scope=0;compute_threaded_recording=0}')
replace('scripts/super_capture_tools.ps1', '    }elseif(-not $Baseline){',
        '''    }elseif($Candidate -eq 'compute-recorder'){
        # Compare the complete new strategy with the actual normal baseline, not the failed
        # bare async-queue candidate. No other rendering candidates or affinity changes.
        $tuning.threaded_recording=1
        if(-not $Baseline){$tuning.asc_queue=1;$tuning.compute_threaded_recording=1}
    }elseif(-not $Baseline){''')
replace('scripts/analyze_super_batch.py',
        '"gds_scope_conflicts_reduced", "gds_scope_barriers_avoided")',
        '"gds_scope_conflicts_reduced", "gds_scope_barriers_avoided", "compute_recorder_submits")')
replace('scripts/analyze_super_batch.py',
        '                                    meter=e["karma_fill_count"], gpu_profile=e["gpu_profile"],',
        '''                                    meter=e["karma_fill_count"], gpu_profile=e["gpu_profile"],
                                    thread_cpu=e.get("thread_cpu"),''')
replace('scripts/analyze_feedback_pair.py',
        '        key = {"feedback-checkpoint": "readback_feedback_checkpoint",',
        '''        if candidate == "compute-recorder" or "compute_threaded_recording" in t.get("requested_tuning", {}):
            expected["compute_threaded_recording"] = 0
        key = {"feedback-checkpoint": "readback_feedback_checkpoint",''')
replace('scripts/analyze_feedback_pair.py', '        else:\n            expected[key] = ordinal',
        '''        elif candidate == "compute-recorder":
            expected.update(threaded_recording=1, asc_queue=ordinal,
                            compute_threaded_recording=ordinal)
        else:
            expected[key] = ordinal''')
replace('scripts/analyze_feedback_pair.py',
        '        if not all(t.get("windows", {}).get(k, {}).get("n", 0) > 0',
        '''        if candidate == "compute-recorder":
            roles = ("command_processor", "graphics_recorder") + (("compute_recorder",) if ordinal else ())
            for role in roles:
                observed = []
                for sample in t.get("snapshots", []):
                    cpu = (sample.get("thread_cpu") or {}).get(role, {})
                    if (0 <= sample.get("seconds", -1) <= 12 and cpu.get("available") is True and
                            isinstance(cpu.get("cpu_percent"), (float, int)) and
                            0 <= cpu["cpu_percent"] <= 200 and
                            0 <= cpu.get("sample_age_ms", 1e9) <= 3000 and
                            0 < cpu.get("window_seconds", 0) < 5):
                        observed.append(cpu)
                if not observed:
                    issues.append(f"Trial {ordinal}: fresh native thread CPU sample missing for {role}.")
        if not all(t.get("windows", {}).get(k, {}).get("n", 0) > 0''')
replace('scripts/analyze_feedback_pair.py', '"cpu-placement": (None, None)}[candidate]',
        '"cpu-placement": (None, None),\n                            "compute-recorder": ("native_compute", "compute_recorder_submits")}[candidate]')
replace('scripts/analyze_feedback_pair.py', '"gds-counter", "cpu-placement") or index.get',
        '"gds-counter", "cpu-placement", "compute-recorder") or index.get')
replace('tests/bootstrap/test_feedback_variants.ps1', '$a.tuning.Count -ne 7 -or $b.tuning.Count -ne 7',
        '$a.tuning.Count -ne 8 -or $b.tuning.Count -ne 8')
replace('tests/bootstrap/test_feedback_variants.ps1', '$variant.tuning.Count -ne 10',
        '$variant.tuning.Count -ne 11')
replace('tests/bootstrap/test_feedback_variants.ps1',
        "Write-Host 'Feedback variant isolation checks passed.'",
        '''Write-Host 'Feedback variant isolation checks passed.'
foreach($baseline in @($true,$false)){
    $variant=Get-SuperFeedbackVariant 'compute-recorder' -Baseline:$baseline
    if($variant.tuning.Count -ne 9 -or $variant.tuning.threaded_recording -ne 1){throw 'Unexpected compute recorder isolation.'}
    foreach($key in $variant.tuning.Keys){
        $expected=if($key -eq 'threaded_recording'){1}elseif($key -in @('asc_queue','compute_threaded_recording')){[int](-not $baseline)}else{0}
        if($variant.tuning[$key] -ne $expected){throw ('Unexpected recorder flag: '+$key)}
    }
}
Write-Host 'Compute recorder strategy is isolated against normal baseline.' ''')
replace('tests/bootstrap/test_feedback_pair.py', '    def test_gds_matches_without_saved_barriers_are_not_an_effect(self):',
        '''    def test_compute_recorder_needs_actual_worker_submissions(self):
        for on, session in enumerate((self.a, self.b)):
            t = session['trials'][0]
            t['requested_tuning'].update(readback_feedback_checkpoint=0, readback_word_load=0,
                gds_counter_scope=0, threaded_recording=1, asc_queue=on,
                compute_threaded_recording=on)
            t['mechanism_delta'] = dict(native_compute=100*on, compute_recorder_submits=0)
            t['snapshots'] = [dict(seconds=5, thread_cpu={role: dict(available=True,
                cpu_percent=75, sample_age_ms=10, window_seconds=1) for role in
                ('command_processor', 'graphics_recorder', 'compute_recorder')})]
        r = qualify(self.a, self.b, 'compute-recorder', 'lightweight')
        self.assertTrue(r['data_checks_passed'])
        self.assertTrue(r['mechanism']['coverage_observed'])
        self.assertFalse(r['mechanism']['scheduling_or_dependency_change_observed'])
        self.b['trials'][0]['mechanism_delta']['compute_recorder_submits'] = 10
        r = qualify(self.a, self.b, 'compute-recorder', 'lightweight')
        self.assertTrue(r['mechanism']['scheduling_or_dependency_change_observed'])
        self.assertFalse(r['performance_acceptance'])
        self.b['trials'][0]['requested_tuning']['compute_threaded_recording'] = 0
        self.assertFalse(qualify(self.a, self.b, 'compute-recorder', 'lightweight')['data_checks_passed'])
        self.b['trials'][0]['requested_tuning']['compute_threaded_recording'] = 1
        self.b['trials'][0]['snapshots'][0]['thread_cpu']['compute_recorder']['sample_age_ms'] = 6000
        self.assertFalse(qualify(self.a, self.b, 'compute-recorder', 'lightweight')['data_checks_passed'])

    def test_gds_matches_without_saved_barriers_are_not_an_effect(self):''')

patch = []
manifest = []
for name, (before, after) in changes.items():
    patch.extend(difflib.unified_diff(before.splitlines(keepends=True), after.splitlines(keepends=True),
                                     fromfile='a/'+name, tofile='b/'+name))
    manifest.append(dict(path=name, original_sha256=hashlib.sha256((ROOT/name).read_bytes()).hexdigest(),
                         prepared_text_sha256=hashlib.sha256(after.encode()).hexdigest()))
out = ROOT/'scripts/patches/compute-recorder-candidate.patch'
if args.apply:
    expected = {v['path']: v['original_sha256'] for v in old_manifest['files']}
    if set(expected) != set(changes):
        raise ValueError('Prepared file set changed; stop and review again.')
    for row in manifest:
        if expected[row['path']] != row['original_sha256']:
            raise ValueError('Source changed since preparation; no files applied: '+row['path'])
out.parent.mkdir(exist_ok=True)
out.write_text(''.join(patch), encoding='utf-8', newline='\n')
if args.apply:
    for name, (_, after) in changes.items():
        newline = '\r\n' if b'\r\n' in (ROOT/name).read_bytes() else '\n'
        (ROOT/name).write_text(after, encoding='utf-8', newline=newline)
(ROOT/'doc/compute-recorder-patch-manifest.json').write_text(json.dumps(dict(
    schema=1, status='applied-awaiting-build' if args.apply else 'prepared-not-applied', patch=str(out.relative_to(ROOT)), files=manifest,
    build_run=False, tests_run=False, game_run=False), indent=2)+'\n', encoding='utf-8')
print(f'{"Applied" if args.apply else "Prepared"} {len(changes)} file changes, {out.stat().st_size} bytes. '
      + ('No build or game started.' if args.apply else 'Build inputs untouched.'))
