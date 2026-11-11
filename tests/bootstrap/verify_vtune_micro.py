# SPDX-License-Identifier: GPL-2.0-or-later
"""Content validation of real, bounded VTune outputs; no game or collector starts."""
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import diagnostic_quality as q

directory = Path(sys.argv[1]).resolve()
if not directory.is_relative_to(ROOT / "state/vtune-micro"):
    raise ValueError("Not an owned micro test directory")
state = q.load(directory / "production-state.json")
backend = state.get("cpu_sampling_backend", "vtune-software")
if backend not in ("vtune-hardware", "vtune-software"):
    raise ValueError("Unrecognized CPU sampling backend")
checks = {"summary": q.vtune_quality(directory, q.utc(state["window_started"]),
                                    q.utc(state["window_finished"]), backend),
          "gameplay": q.vtune_gameplay_cpu_quality(directory, state, state["target_pid"],
                                                  cpu_backend=backend)}
for window in state["super_qualified_windows"]:
    checks[f"cycle_{window['cycle']}"] = q.vtune_gameplay_cpu_quality(
        directory, window, state["target_pid"],
        f"vtune-super-cycle-{window['cycle']}-hotspots", backend)
q.write_json(directory / "verified-cpu-reports.json", checks)
print(json.dumps({"validated": True, "reports": len(checks), "cpu_sampling_backend": backend,
                  "scope": "native micro; no game/GPU coverage claimed"}))
