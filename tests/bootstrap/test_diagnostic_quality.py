# SPDX-License-Identifier: GPL-2.0-or-later
import csv
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import diagnostic_quality as quality


class DiagnosticQualityTest(unittest.TestCase):
    def setUp(self):
        base = ROOT / "state/offline-tests"
        base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.start = 1767225600

    def write(self, name, value):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value, encoding="utf-8")
        return path

    def test_detach_receipt_cannot_mask_failed_collector(self):
        receipt = {"sequence": 42, "action": "vtune-detach", "accepted": True,
                   "error": "", "applied_unix_ns": 1767225601000000000}
        self.write("vtune-control.json", json.dumps(receipt))
        state = {"vtune_detach_sequence": 42, "vtune_stop_method": "native-itt-detach",
                 "vtune_exit_code": -1, "vtune_detached_utc": "2026-01-01T00:00:01Z"}
        with self.assertRaisesRegex(ValueError, "completion evidence"):
            quality.vtune_control_quality(self.root, state)
        state["vtune_exit_code"] = 0
        self.assertEqual(quality.vtune_control_quality(self.root, state)["native_receipt_sequence"], 42)
        receipt["sequence"] = 41
        self.write("vtune-control.json", json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, "completion evidence"):
            quality.vtune_control_quality(self.root, state)

    def test_profiler_exit_requires_successful_exports_and_engine_shutdown(self):
        state = {"nsight_exit_code": 0, "vtune_exit_code": 0, "window_finished": "2026-01-01T00:01:00Z"}
        ready = {"pid": 42, "policy": "engine-shutdown-self-terminate", "nsight_exit_code": 0, "vtune_completed": True}
        receipt = {"pid": 42, "policy": ready["policy"], "shutdown_completed": True, "exit_code": 0,
                   "exit_unix_ns": 1767225661000000000}
        self.write("exit-ready.json", json.dumps(ready))
        self.write("exit-completed.json", json.dumps(receipt))
        self.assertTrue(quality.diagnostic_exit_quality(self.root, state, 42)["engine_shutdown_completed"])
        state["nsight_exit_code"] = -1
        with self.assertRaisesRegex(ValueError, "completion or engine shutdown"):
            quality.diagnostic_exit_quality(self.root, state, 42)
        state["nsight_exit_code"] = 0
        receipt["shutdown_completed"] = False
        self.write("exit-completed.json", json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, "completion or engine shutdown"):
            quality.diagnostic_exit_quality(self.root, state, 42)
        receipt["shutdown_completed"] = True
        receipt["pid"] = 41
        self.write("exit-completed.json", json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, "completion or engine shutdown"):
            quality.diagnostic_exit_quality(self.root, state, 42)

    def wpr_text(self):
        return (
            "Start time (UTC) : 2026/01/01:00:00:00.0000000\n"
            "End time (UTC) : 2026/01/01:00:01:10.0000000\n"
            "Total # Lost Buffers : 0\nTotal # Lost Events : 0\n"
            "{" + quality.DXG_GUID + "} 25 400 Microsoft-Windows-DxgKrnl\n"
            "0x2e 0x00 0x02 10 160 Sampled Profile\n"
            "0x24 0x00 0x05 100 2800 Thread: CSwitch\n"
            "0xb2 0x09 0x01 0x00 0x11 0x00 0x4000000000000001 10 100 Microsoft-Windows-DxgKrnl/QueuePacket/win:Start\n"
            "0xaf 0x08 0x01 0x00 0x11 0x00 0x4000000000000001 5 100 Microsoft-Windows-DxgKrnl/DmaPacket/win:Start\n"
        )

    def test_wpr_actual_events_and_time(self):
        result = quality.wpr_quality(self.wpr_text(), self.start + 5, self.start + 65)
        self.assertEqual(result["gpu_kernel_events"], 25)
        self.assertEqual(result["cpu_samples"], 10)

    def test_wpr_rejects_missing_gpu_with_valid_file(self):
        text = self.wpr_text().replace("{" + quality.DXG_GUID + "} 25 400 Microsoft-Windows-DxgKrnl\n", "")
        with self.assertRaisesRegex(ValueError, "no DxgKrnl"):
            quality.wpr_quality(text, self.start + 5, self.start + 65)

    def test_wpr_rejects_losses_and_short_capture(self):
        with self.assertRaisesRegex(ValueError, "lost events"):
            quality.wpr_quality(self.wpr_text().replace("Lost Events : 0", "Lost Events : 1"), self.start, self.start + 60)
        with self.assertRaisesRegex(ValueError, "does not cover"):
            quality.wpr_quality(self.wpr_text(), self.start, self.start + 80)

    def test_wpr_startup_and_other_process_samples_cannot_mask_gameplay_gap(self):
        state = {"window_started": "2026-01-01T00:00:05Z", "window_finished": "2026-01-01T00:01:05Z"}
        self.write("wpr-gameplay-cpu.json", json.dumps({**state, "target_pid": 42, "method": "xperf-profile-gameplay-range"}))
        path = self.write("wpr-gameplay-cpu.txt", " Idle ( 0), 5000, 100.00, idle\nTotal Weight: 0\n")
        with self.assertRaisesRegex(ValueError, "no target CPU samples"):
            quality.wpr_gameplay_quality(self.root, state, 42)
        path.write_text("other ( 43), 5000, 100.00, module\nTotal Weight: 5000\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "no target CPU samples"):
            quality.wpr_gameplay_quality(self.root, state, 42)
        path.write_text("inFAMOUS Second Son ( 42), 5000, 100.00, module\nTotal Weight: 5000\n", encoding="utf-8")
        self.assertEqual(quality.wpr_gameplay_quality(self.root, state, 42)["target_gameplay_profile_weight"], 5000)
        state["window_finished"] = "2026-01-01T00:01:06Z"
        with self.assertRaisesRegex(ValueError, "stale"):
            quality.wpr_gameplay_quality(self.root, state, 42)

    def test_vtune_cpu_requires_target_samples_in_its_gameplay_filter(self):
        state = {"window_started": "2026-01-01T00:00:05Z", "window_finished": "2026-01-01T00:01:05Z"}
        scope = {**state, "target_pid": 42, "collection_start_utc": "2026-01-01T00:00:00Z",
                 "begin_seconds": 6, "end_seconds": 64}
        self.write("vtune-gameplay-hotspots.json", json.dumps(scope))
        path = self.write("vtune-gameplay-hotspots.csv", "PID,Function,CPU Time,Module\n43,other,20,module\n")
        with self.assertRaisesRegex(ValueError, "no target CPU samples"):
            quality.vtune_gameplay_cpu_quality(self.root, state, 42)
        path.write_text("PID,Function,CPU Time,Module\n42,work,1.5,module\n", encoding="utf-8")
        result = quality.vtune_gameplay_cpu_quality(self.root, state, 42)
        self.assertEqual(result["sampled_cpu_seconds"], 1.5)
        self.assertFalse(result["wpr_timer_sampling_available"])
        scope["begin_seconds"] = 0
        self.write("vtune-gameplay-hotspots.json", json.dumps(scope))
        with self.assertRaisesRegex(ValueError, "time filter"):
            quality.vtune_gameplay_cpu_quality(self.root, state, 42)
        scope["begin_seconds"] = 6
        scope["target_pid"] = 41
        self.write("vtune-gameplay-hotspots.json", json.dumps(scope))
        with self.assertRaisesRegex(ValueError, "stale"):
            quality.vtune_gameplay_cpu_quality(self.root, state, 42)

    def test_wpr_conflict_exception_does_not_apply_to_normal_cpu_capture(self):
        text = self.wpr_text().replace("0x2e 0x00 0x02 10 160 Sampled Profile\n", "")
        with self.assertRaisesRegex(ValueError, "cpu_samples"):
            quality.wpr_quality(text, self.start + 5, self.start + 65)
        self.assertFalse(quality.wpr_quality(text, self.start + 5, self.start + 65, False)["timer_cpu_sampling_required"])

    def frames(self):
        self.write("clock.json", json.dumps({"qpc_frequency": 10000000}))
        return self.write("frames.csv",
            "frame,elapsed_ns,interval_ns,utc_ns,qpc_before,qpc_after\n"
            f"0,0,0,{self.start * 1000000000},100,101\n"
            f"1,1000000000,1000000000,{(self.start + 1) * 1000000000},10000100,10000101\n"
            f"2,2000000000,1000000000,{(self.start + 2) * 1000000000},20000100,20000101\n")

    def test_frames_pair_utc_qpc(self):
        self.frames()
        result = quality.frame_quality(self.root, self.start + .1, self.start + 1.9)
        self.assertEqual(result["rows"], 3)
        self.assertEqual(result["wall_clock_steps_over_5ms"], 0)
        self.assertEqual(result["max_clock_read_bracket_ns"], 100)

    def test_frames_reject_missing_clock_and_bad_intervals(self):
        path = self.frames()
        path.write_text(path.read_text().replace("2,2000000000,1000000000", "2,2000000000,500000000"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "inconsistent intervals"):
            quality.frame_quality(self.root, self.start + .1, self.start + 1.9)

    def test_frames_reports_wall_clock_step(self):
        path = self.frames()
        text = path.read_text().replace(str((self.start + 2) * 1000000000), str((self.start + 3) * 1000000000))
        path.write_text(text, encoding="utf-8")
        self.assertEqual(quality.frame_quality(self.root, self.start + .1, self.start + 1.9)["wall_clock_steps_over_5ms"], 1)

    def sensors(self):
        header = "utc,query_start_qpc,query_end_qpc," + ",".join("field" + str(i) for i in range(10)) + "\n"
        rows = []
        for second in range(61):
            stamp = "2026-01-01T00:" + ("01:00" if second == 60 else f"00:{second:02}") + "Z"
            rows.append(stamp + ",10,11," + ",".join(["1"] * 10) + "\n")
        return self.write("nvidia.csv", header + "".join(rows))

    def test_large_nsight_names_stream_without_changing_shape(self):
        path = self.write("metrics.xls", "event\tvalue\n" + "x" * 200000 + "\t1.5\n")
        rows = quality.table_rows(path, "\t")
        self.assertEqual(next(rows), ["event", "value"])
        self.assertEqual(len(next(rows)[0]), 200000)
        self.assertEqual(list(rows), [])

    def test_sensor_complete_rows(self):
        self.assertEqual(quality.sensor_quality(self.sensors(), self.start + 5, self.start + 60)["complete_rows"], 61)

    def test_sensor_reject_truncated_tail(self):
        path = self.sensors()
        with path.open("a") as stream:
            stream.write("2026-01-01T00:01:01Z,30,31,1,2")
        with self.assertRaisesRegex(ValueError, "incomplete"):
            quality.sensor_quality(path, self.start + 5, self.start + 60)

    def test_sensor_reject_missing_time_coverage(self):
        with self.assertRaisesRegex(ValueError, "cover"):
            quality.sensor_quality(self.sensors(), self.start + 5, self.start + 80)

    def test_sensor_reject_interior_hole_despite_valid_endpoints(self):
        path = self.sensors()
        lines = path.read_text().splitlines()
        path.write_text("\n".join(lines[:12] + lines[30:]) + "\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "sampling gap"):
            quality.sensor_quality(path, self.start + 5, self.start + 60)

    def test_sensor_uses_snapshot_time_not_late_observer_time(self):
        path = self.sensors()
        lines = path.read_text().splitlines()
        lines[0] = lines[0].replace("field0", "timestamp")
        for index in range(1, len(lines)):
            cols = lines[index].split(",")
            second = index - 1
            cols[3] = "2026/01/01 08:" + ("01:00.000" if second == 60 else f"00:{second:02}.000")
            if index == len(lines) - 1:
                cols[0] = "2026-01-01T00:02:00Z"
            lines[index] = ",".join(cols)
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        result = quality.sensor_quality(path, self.start + 5, self.start + 60, 8 * 3600)
        self.assertEqual(result["last_unix_s"], self.start + 60)
        self.assertEqual(result["max_window_gap_seconds"], 1)

    def vtune(self):
        self.write("vtune-summary.csv",
            "Hierarchy Level,Metric Name,Metric Value\n1,CPU Time,100\n"
            "1,Collection start time,00:00:00 01/01/2026 UTC\n"
            "1,Collection stop time,00:01:10 01/01/2026 UTC\n"
            '0,"DRAM, GB/sec",55,51,20,30\n')
        self.write("vtune-errors.log", "Peak bandwidth measurement finished.")

    def test_vtune_calibration_and_bounds(self):
        self.vtune()
        self.assertEqual(quality.vtune_quality(self.root, self.start + 5, self.start + 65)["peak_gbps"], 55)

    def test_vtune_reject_uncalibrated_or_bad_scale(self):
        self.vtune()
        self.write("vtune-errors.log", "Measuring peak DRAM bandwidth is not enabled")
        with self.assertRaisesRegex(ValueError, "not calibrated"):
            quality.vtune_quality(self.root, self.start + 5, self.start + 65)
        self.write("vtune-errors.log", "Peak bandwidth measurement finished.")
        path = self.root / "vtune-summary.csv"
        path.write_text(path.read_text().replace(",55,51,", ",25,51,"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "inconsistent"):
            quality.vtune_quality(self.root, self.start + 5, self.start + 65)

    def test_nsight_requires_decoded_requested_frames(self):
        capture = "user/captures/nsight/"
        self.write(capture + "frame.ngfx-gputrace", "raw trace fixture")
        self.write(capture + "BASE_UNLOCKED/D3DPERF_EVENTS.xls", "event\ttime_ms\nDraw\t1.0\n")
        self.write(capture + "BASE_UNLOCKED/GPUTRACE_FRAME.xls", "metric\t1.0\n")
        self.write("diagnostics/nsight-output.log", "")
        state = {"nsight_sdk_accepted": True,
                 "nsight_scene_anchor_utc": "2026-01-01T00:00:00Z"}
        with self.assertRaisesRegex(ValueError, "fewer detailed frames"):
            quality.nsight_quality(self.root, state, 3)
        self.write(capture + "BASE_UNLOCKED/GPUTRACE_FRAME.xls", "metric\t1.0\t2.0\t3.0\n")
        self.assertEqual(quality.nsight_quality(self.root, state, 3)["captured_frames"], 3)
        state["nsight_sdk_accepted"] = False
        with self.assertRaisesRegex(ValueError, "start acknowledgement"):
            quality.nsight_quality(self.root, state, 3)


if __name__ == "__main__":
    unittest.main()
