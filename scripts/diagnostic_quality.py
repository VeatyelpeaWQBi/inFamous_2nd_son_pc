# SPDX-License-Identifier: GPL-2.0-or-later
"""Content and time coverage checks; never start a game or change recorded artifacts."""
import argparse
import csv
from datetime import datetime, timezone, timedelta
import hashlib
import json
import math
from pathlib import Path
import re
import struct

from project_paths import project_child, write_json

DXG_GUID = "802ec45a-1e99-4b83-9920-87c98277ba9d"


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def utc(value):
    return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()


def table_rows(path, delimiter=","):
    # Nsight's flattened event names can be long. Keep only one row in memory.
    csv.field_size_limit(16 * 1024 * 1024)
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.reader(stream, delimiter=delimiter)
        first = next(reader, None)
        if first is None:
            raise ValueError(f"{path.name}: empty table")
        width = len(first)
        yield first
        for row in reader:
            if len(row) != width:
                raise ValueError(f"{path.name}: incomplete or changing columns")
            yield row


def table(path, delimiter=","):
    return list(table_rows(path, delimiter))


def wpr_quality(text, begin, end, require_cpu_samples=True):
    result = {}
    for kind in ("Buffers", "Events"):
        match = re.search(r"Total # Lost " + kind + r"\s*:\s*(\d+)", text)
        if not match:
            raise ValueError("WPR: missing loss counters")
        result["lost_" + kind.lower()] = int(match[1])
        if int(match[1]):
            raise ValueError("WPR: lost " + kind.lower())
    providers = {
        guid.lower(): int(count)
        for guid, count in re.findall(r"^\{([0-9a-fA-F-]+)\}\s+(\d+)\s+\d+\s+.*$", text, re.M)
    }
    result["gpu_kernel_events"] = providers.get(DXG_GUID, 0)
    if not result["gpu_kernel_events"]:
        raise ValueError("WPR: no DxgKrnl GPU kernel events; GPU scheduling coverage is missing")
    for field, task in (("gpu_queue_events", "QueuePacket"), ("gpu_dma_events", "DmaPacket")):
        result[field] = sum(map(int, re.findall(
            r"(\d+)\s+\d+\s+Microsoft-Windows-DxgKrnl/" + task + r"/", text)))
        if not result[field]:
            raise ValueError("WPR: missing " + field)
    for field, pattern in (("cpu_samples", "Sampled Profile"), ("context_switches", "Thread: CSwitch")):
        counts = re.findall(r"(\d+)\s+\d+\s+" + re.escape(pattern) + r"\s*$", text, re.M)
        result[field] = sum(map(int, counts))
        if not result[field] and (field != "cpu_samples" or require_cpu_samples):
            raise ValueError("WPR: missing " + field)
    result["timer_cpu_sampling_required"] = require_cpu_samples
    for label in ("Start", "End"):
        match = re.search(label + r" time \(UTC\)\s*:\s*(\d{4}/\d\d/\d\d:\d\d:\d\d:\d\d)\.(\d+)", text)
        if not match:
            raise ValueError("WPR: missing trace time bounds")
        timestamp = datetime.strptime(match[1], "%Y/%m/%d:%H:%M:%S").replace(tzinfo=timezone.utc).timestamp()
        result[label.lower() + "_unix_s"] = timestamp + float("0." + match[2])
    if result["start_unix_s"] > begin or result["end_unix_s"] < end:
        raise ValueError("WPR: trace does not cover the gameplay window")
    return result


def wpr_gameplay_quality(directory, state, target_pid):
    scope = load(directory / "wpr-gameplay-cpu.json")
    if (scope.get("target_pid") != target_pid or
            any(scope.get(key) != state[key] for key in ("window_started", "window_finished"))):
        raise ValueError("WPR: stale gameplay CPU report")
    text = (directory / "wpr-gameplay-cpu.txt").read_text(encoding="utf-8-sig")
    weights = re.findall(r"^\s*[^,\n]+\(\s*" + str(target_pid) + r"\)\s*,\s*(\d+)\s*,", text, re.M)
    weight = sum(map(int, weights))
    if not weight:
        raise ValueError("WPR: no target CPU samples inside the gameplay window; startup samples cannot prove coverage")
    return {"target_gameplay_profile_weight": weight,
            "target_gameplay_cpu_sampled": True, "method": scope["method"]}


def wpr_full_quality(directory, state, target_pid, begin, end, cpu_backend=None):
    result = wpr_quality((directory / "wpr-statistics.txt").read_text(encoding="utf-8-sig"),
                         begin, end, require_cpu_samples=cpu_backend != "vtune-hardware")
    if cpu_backend != "vtune-hardware":
        result.update(wpr_gameplay_quality(directory, state, target_pid))
    else:
        result["scope"] = "GPU, scheduling, wait stacks, memory and I/O; CPU instruction sampling belongs to VTune"
    return result


def vtune_gameplay_cpu_quality(directory, state, target_pid):
    scope = load(directory / "vtune-gameplay-hotspots.json")
    if (scope.get("target_pid") != target_pid or
            any(scope.get(key) != state[key] for key in ("window_started", "window_finished"))):
        raise ValueError("VTune CPU: stale gameplay report")
    start = utc(scope["collection_start_utc"])
    begin, end = float(scope["begin_seconds"]), float(scope["end_seconds"])
    expected_begin = max(0, utc(state["window_started"]) - start + 1)
    expected_end = utc(state["window_finished"]) - start - 1
    if (not all(math.isfinite(value) for value in (begin, end)) or end <= begin or
            abs(begin - expected_begin) > .001 or abs(end - expected_end) > .001):
        raise ValueError("VTune CPU: gameplay time filter does not match the recorded window")
    rows = table_rows(directory / "vtune-gameplay-hotspots.csv")
    header = next(rows)
    required = ("PID", "Function", "CPU Time", "Module")
    if any(key not in header for key in required):
        raise ValueError("VTune CPU: missing target/function sample columns")
    count = 0
    cpu_time = 0.0
    for values in rows:
        row = dict(zip(header, values))
        if int(row["PID"]) != target_pid:
            continue
        value = float(row["CPU Time"])
        if not math.isfinite(value) or value < 0:
            raise ValueError("VTune CPU: invalid sampled CPU time")
        if value > 0:
            count += 1
            cpu_time += value
    if not count or cpu_time <= 0:
        raise ValueError("VTune CPU: no target CPU samples inside the gameplay window")
    return {"source": "VTune hardware event-based sampling", "target_pid": target_pid,
            "positive_hotspot_rows": count, "sampled_cpu_seconds": cpu_time,
            "report_window_seconds": end - begin, "utc_correlation_uncertainty_seconds": 1,
            "wpr_timer_sampling_available": False}


def frame_quality(session, begin, end):
    clock = load(session / "clock.json")
    frequency = int(clock["qpc_frequency"])
    if frequency <= 0:
        raise ValueError("Frames: invalid QPC frequency")
    previous = None
    first = last = None
    count = 0
    max_bracket = 0
    clock_steps = 0
    with (session / "frames.csv").open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            values = tuple(int(row[key]) for key in
                           ("frame", "elapsed_ns", "interval_ns", "utc_ns", "qpc_before", "qpc_after"))
            index, elapsed, interval, wall, before, after = values
            if after < before or wall <= 0:
                raise ValueError("Frames: invalid paired clock sample")
            if previous is None:
                if values[:3] != (0, 0, 0):
                    raise ValueError("Frames: first frame must start at zero")
                first = wall / 1e9
            else:
                if index != previous[0] + 1 or elapsed <= previous[1] or interval != elapsed - previous[1]:
                    raise ValueError("Frames: discontinuity or inconsistent intervals")
                if before < previous[5]:
                    raise ValueError("Frames: QPC moved backwards")
                wall_delta = (wall - previous[3]) / 1e9
                qpc_delta = (before - previous[4]) / frequency
                if abs(wall_delta - qpc_delta) > .005:
                    clock_steps += 1
            count += 1
            last = wall / 1e9
            max_bracket = max(max_bracket, (after - before) * 1e9 / frequency)
            previous = values
    if count < 2 or first > begin or last < end:
        raise ValueError("Frames: no absolute-clock coverage of the gameplay window")
    return {"rows": count, "qpc_frequency": frequency, "first_unix_s": first,
            "last_unix_s": last, "max_clock_read_bracket_ns": max_bracket,
            "wall_clock_steps_over_5ms": clock_steps,
            "correlation": "per-frame QPC bracket and precise UTC; not hardware scanout"}


def sensor_quality(path, begin, end, utc_offset_seconds=None):
    rows = table(path)
    header = [field.strip() for field in rows[0]]
    if len(header) != 13 or header[:2] != ["utc", "query_start_qpc"] or header[2] not in ("query_end_qpc", "query_observed_end_qpc"):
        raise ValueError("Sensors: missing UTC/QPC clock columns")
    times = []
    clock_source = "query-process-exit UTC"
    use_snapshot_clock = utc_offset_seconds is not None and header[3] == "timestamp"
    if use_snapshot_clock:
        clock_source = "NVML snapshot local timestamp with recorded launch UTC offset"
    for row in rows[1:]:
        if use_snapshot_clock:
            moment = datetime.strptime(row[3].strip(), "%Y/%m/%d %H:%M:%S.%f")
            times.append(moment.replace(tzinfo=timezone(timedelta(seconds=utc_offset_seconds))).timestamp())
        else:
            times.append(utc(row[0]))
        if int(row[2]) < int(row[1]):
            raise ValueError("Sensors: QPC moved backwards")
    if len(times) < 2 or any(b < a for a, b in zip(times, times[1:])):
        raise ValueError("Sensors: insufficient or unordered data")
    if times[0] > begin or times[-1] < end - 3:
        raise ValueError("Sensors: samples do not cover the gameplay window")
    window_times = [begin] + [stamp for stamp in times if begin < stamp < end] + [end]
    window_gap = max(b - a for a, b in zip(window_times, window_times[1:]))
    if window_gap > 5:
        raise ValueError("Sensors: a sampling gap longer than 5 seconds intersects the gameplay window")
    return {"complete_rows": len(times), "first_unix_s": times[0],
            "last_unix_s": times[-1], "scope": "whole GPU; not process-exclusive",
            "max_gap_seconds": max(b - a for a, b in zip(times, times[1:])),
            "max_window_gap_seconds": window_gap, "utc_source": clock_source,
            "qpc_end_is_observation_bound": True}


def vtune_quality(directory, begin, end):
    # The summary deliberately contains several CSV sections of different widths.
    with (directory / "vtune-summary.csv").open(encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.reader(stream))
    values = {row[1]: row[2] for row in rows if len(row) == 3}
    result = {}
    for label in ("start", "stop"):
        value = values["Collection " + label + " time"]
        result[label + "_unix_s"] = datetime.strptime(value, "%H:%M:%S %d/%m/%Y UTC").replace(tzinfo=timezone.utc).timestamp()
    if result["start_unix_s"] > begin or result["stop_unix_s"] < end - 1:
        raise ValueError("VTune: sampling did not cover the gameplay window")
    warning = "Measuring peak DRAM bandwidth is not enabled"
    log = (directory / "vtune-errors.log").read_text(encoding="utf-8-sig", errors="replace")
    if warning in log:
        raise ValueError("VTune: DRAM peak bandwidth was not calibrated")
    bandwidth = next(row for row in rows if len(row) >= 6 and row[1] == "DRAM, GB/sec")
    peak, observed = float(bandwidth[2]), float(bandwidth[3])
    if not math.isfinite(peak) or peak <= 0 or observed > peak * 1.2:
        raise ValueError("VTune: bandwidth scale is inconsistent with observed bandwidth")
    measured = "Peak bandwidth measurement finished." in log
    if not measured:
        raise ValueError("VTune: no completed peak bandwidth measurement was recorded")
    result.update(calibration_requested=True, calibration_measurement_finished=measured,
                  peak_gbps=peak, observed_gbps=observed,
                  raw_cpu_time_seconds=float(values["CPU Time"]),
                  attach_calibration_uncertainty="may be not accurate in the attach mode" in log)
    return result


def diagnostic_exit_quality(directory, state, target_pid):
    ready = load(directory / "exit-ready.json")
    receipt = load(directory / "exit-completed.json")
    policy = "engine-shutdown-self-terminate"
    if (ready.get("pid") != target_pid or receipt.get("pid") != target_pid or
            ready.get("policy") != policy or receipt.get("policy") != policy or
            ready.get("nsight_exit_code") != 0 or ready.get("vtune_completed") is not True or
            state.get("nsight_exit_code") != 0 or state.get("vtune_exit_code") != 0 or
            receipt.get("shutdown_completed") is not True or receipt.get("exit_code") != 0 or
            receipt.get("exit_unix_ns", 0) / 1e9 < utc(state["window_finished"])):
        raise ValueError("Diagnostic exit: collector completion or engine shutdown evidence is invalid")
    return {"policy": policy, "engine_shutdown_completed": True, "collectors_exported_before_exit": True,
            "limitation": "reproduced Nsight DLL detach fault bypassed; third-party teardown is not repaired"}


def vtune_control_quality(directory, state):
    receipt = load(directory / "vtune-control.json")
    if (state.get("vtune_exit_code") != 0 or
            state.get("vtune_stop_method") != "native-itt-detach" or
            receipt.get("sequence") != state.get("vtune_detach_sequence") or
            receipt.get("action") != "vtune-detach" or
            receipt.get("accepted") is not True or receipt.get("error") or
            receipt.get("applied_unix_ns", 0) <= 0 or not state.get("vtune_detached_utc")):
        raise ValueError("VTune: native detach or collector completion evidence is invalid")
    return {"method": "native-itt-detach", "collector_exit_code": 0,
            "native_receipt_sequence": receipt["sequence"],
            "detached_utc": state["vtune_detached_utc"]}


def nsight_quality(session, state, requested_frames):
    if not state.get("nsight_sdk_accepted"):
        raise ValueError("Nsight: missing native start acknowledgement")
    start = utc(state["nsight_scene_anchor_utc"])
    capture = session / "user/captures/nsight"
    if not any(p.stat().st_size for p in capture.glob("*.ngfx-gputrace")):
        raise ValueError("Nsight: no saved GPU trace")
    event_rows = metrics = captured_frames = 0
    tables = {}
    for path in capture.rglob("*.xls"):
        rows = table_rows(path, "\t")
        row_count = column_count = 0
        if path.name == "D3DPERF_EVENTS.xls":
            header = next(rows)
            row_count, column_count = 1, len(header)
            for row in rows:
                if not math.isfinite(float(row[1])):
                    raise ValueError("Nsight: invalid event duration")
                event_rows += 1
                row_count += 1
        elif path.name == "GPUTRACE_FRAME.xls":
            for row in rows:
                if not all(math.isfinite(float(value)) for value in row[1:]):
                    raise ValueError("Nsight: invalid hardware metric")
                if captured_frames and captured_frames != len(row) - 1:
                    raise ValueError("Nsight: frame count changed within the metric table")
                captured_frames = len(row) - 1
                row_count += 1
                column_count = len(row)
                metrics += 1
        else:
            for row in rows:
                row_count += 1
                column_count = len(row)
        tables[path.relative_to(capture).as_posix()] = {"rows": row_count, "columns": column_count}
    if not event_rows or not metrics:
        raise ValueError("Nsight: no decoded GPU events/metrics")
    if captured_frames < requested_frames:
        raise ValueError("Nsight: fewer detailed frames than requested")
    log = (session / "diagnostics/nsight-output.log").read_text(encoding="utf-8-sig", errors="replace")
    return {"sdk_start_unix_s": start, "captured_frames": captured_frames,
            "requested_frames": requested_frames, "event_rows": event_rows, "tables": tables,
            "frame_metric_rows": metrics, "periodic_samples_merged": "samples merged" in log,
            "scope": "detailed frame snapshot; continuous GPU scheduling is recorded by ETW",
            "coverage_evidence": "native SDK start receipt and decoded per-frame hardware metrics"}


def symbols_quality(project, launch):
    receipt = load(project / Path(launch["runtime"]).parent / "build-receipt.json")
    runtime = project_child(launch["runtime"])
    pdb = project_child(receipt["pdb"])
    def digest(path):
        with path.open("rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()
    if digest(runtime) != launch["runtime_sha256"] or digest(pdb) != receipt["pdb_sha256"]:
        raise ValueError("Symbols: recorded runtime or PDB has changed")
    # MSF stream 1 stores the PDB GUID and age; compare with PE CodeView RSDS.
    with pdb.open("rb") as stream:
        header = stream.read(56)
        if not header.startswith(b"Microsoft C/C++ MSF 7.00"):
            raise ValueError("Symbols: unsupported PDB format")
        block, _, _, directory_size, _, block_map = struct.unpack_from("<6I", header, 32)
        stream.seek(block_map * block)
        pages = struct.unpack("<%dI" % ((directory_size + block - 1) // block),
                              stream.read(4 * ((directory_size + block - 1) // block)))
        directory = b""
        for page in pages:
            stream.seek(page * block)
            directory += stream.read(block)
        streams = struct.unpack_from("<I", directory)[0]
        sizes = struct.unpack_from("<%dI" % streams, directory, 4)
        offset = 4 + streams * 4
        pdb_info = None
        for index, size in enumerate(sizes):
            count = 0 if size == 0xffffffff else (size + block - 1) // block
            pages = struct.unpack_from("<%dI" % count, directory, offset) if count else ()
            offset += 4 * count
            if index == 1:
                stream.seek(pages[0] * block)
                pdb_info = stream.read(28)
                break
    with runtime.open("rb") as stream:
        stream.seek(0x3c);pe = struct.unpack("<I", stream.read(4))[0]
        stream.seek(pe + 6);sections = struct.unpack("<H", stream.read(2))[0]
        stream.seek(pe + 20);optional_size = struct.unpack("<H", stream.read(2))[0]
        stream.seek(pe + 24 + 112 + 6 * 8);debug_rva, debug_size = struct.unpack("<II", stream.read(8))
        stream.seek(pe + 24 + optional_size)
        section_data = stream.read(sections * 40)
        debug_offset = None
        for index in range(sections):
            virtual_size, address, raw_size, raw = struct.unpack_from("<4I", section_data, index * 40 + 8)
            if address <= debug_rva < address + max(virtual_size, raw_size):
                debug_offset = raw + debug_rva - address
        if debug_offset is None:
            raise ValueError("Symbols: missing CodeView directory")
        stream.seek(debug_offset);entries = stream.read(debug_size)
        for offset in range(0, debug_size, 28):
            kind, size, _, raw = struct.unpack_from("<4I", entries, offset + 12)
            if kind == 2:
                stream.seek(raw);codeview = stream.read(size)
                if codeview[:4] == b"RSDS" and codeview[4:20] == pdb_info[12:28] and codeview[20:24] == pdb_info[8:12]:
                    return {"pdb_guid_age_match": True, "runtime_sha256_match": True,
                            "pdb_sha256_match": True}
    raise ValueError("Symbols: EXE and PDB identity mismatch")


def inspect(session, project):
    directory = session / "diagnostics"
    state, plan, launch = (load(directory / "status.json"), load(directory / "plan.json"),
                           load(session / "launch.json"))
    if not state.get("window_started") or not state.get("window_finished"):
        return {"schema": 1, "complete": False, "status": "incomplete", "checks": {},
                "errors": ["No completed gameplay window was recorded"], "limitations": []}
    begin, end = utc(state["window_started"]), utc(state["window_finished"])
    result = {"schema": 1, "validator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "target_pid": launch["pid"], "window_seconds": end - begin,
              "checks": {}, "errors": [], "limitations": [
                  "Heavy instrumentation results are not normal-game FPS acceptance.",
                  "Guest code and anonymous/JIT samples need a guest address map.",
                  "Scene/brightness/particles require separate visual and semantic acceptance."]}
    checks = {
        "wpr": lambda: wpr_full_quality(directory, state, launch["pid"], begin, end, plan.get("cpu_sampling_backend")),
        "frames": lambda: frame_quality(session, begin, end),
        "symbols": lambda: symbols_quality(project, launch),
        "vtune": lambda: vtune_quality(directory, begin, end),
    }
    if any(tool.get("name") == "nvidia_smi" and tool.get("available") for tool in plan.get("tools", [])):
        offset = datetime.fromisoformat(launch["prepared_at"]).utcoffset().total_seconds()
        checks["sensors"] = lambda: sensor_quality(directory / "nvidia.csv", begin, end, offset)
    else:
        result["limitations"].append("NVIDIA device telemetry was not available on this host.")
    if plan["gpu_capture_backend"] == "nsight":
        checks["nsight"] = lambda: nsight_quality(session, state, plan.get("nsight_detail_frames", 3))
        if plan.get("validation_scope"):
            result["limitations"].append("Vulkan validation scope: " + plan["validation_scope"])
    if plan.get("nsight_exit_policy") == "engine-shutdown-self-terminate":
        checks["diagnostic_exit"] = lambda: diagnostic_exit_quality(directory, state, launch["pid"])
        result["limitations"].append("Nsight diagnostics exit after successful collector export and engine shutdown using self-termination; the reproduced third-party DLL detach fault is bypassed, not repaired.")
    if plan.get("cpu_sampling_backend") == "vtune-hardware":
        checks["cpu_hotspots"] = lambda: vtune_gameplay_cpu_quality(directory, state, launch["pid"])
        result["limitations"].append("Concurrent VTune hardware sampling suppresses WPR timer CPU sampling; CPU hotspots must pass a separate target-PID and gameplay-window check using VTune. ETW wait/scheduling stacks remain enabled.")
    if plan.get("vtune_stop_method") == "native-itt-detach":
        checks["vtune_control"] = lambda: vtune_control_quality(directory, state)
    for name, check in checks.items():
        try:
            result["checks"][name] = {"validated": True, **check()}
        except (ValueError, KeyError, OSError, StopIteration, struct.error, csv.Error) as error:
            message = f"{name}: {error}"
            result["checks"][name] = {"validated": False, "error": message}
            result["errors"].append(message)
    if result["checks"].get("vtune", {}).get("attach_calibration_uncertainty"):
        result["limitations"].append("VTune measured peak bandwidth in attach mode; concurrent system load may bias the scale.")
    if result["checks"].get("nsight", {}).get("periodic_samples_merged"):
        result["limitations"].append("Nsight merged periodic samples; sampling resolution is limited.")
    result["complete"] = not result["errors"]
    result["status"] = "complete-with-stated-limits" if result["complete"] else "incomplete"
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--session", required=True)
    args = parser.parse_args()
    session = project_child(args.session)
    if not session.is_relative_to(project_child("state/sessions")):
        parser.error("Quality inspection must stay in project sessions")
    result = inspect(session, Path(__file__).resolve().parents[1])
    write_json(session / "diagnostics/data-quality.json", result)
    print(json.dumps({"complete": result["complete"], "errors": result["errors"]}))
    # Persist quality findings; the caller includes them in the collection outcome.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
