"""Render measured native cadence and the Full detail-capture anchor separately."""
import json
import os
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "doc/super-deep-20261031"
os.environ.setdefault("MPLCONFIGDIR", str(OUT / "plot-config"))
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

a = json.loads((ROOT / "doc/super-analysis-20261031.json").read_text())
native, full = a["sessions"]
light = [t for t in native["trials"] if t["profile"] == "lightweight" and t["status"] == "collected"]
fig, axes = plt.subplots(3, 1, figsize=(10, 10), constrained_layout=True)
ax = axes[0]
for t in light:
    ax.plot([i+.5 for i in range(30)], [r.get("fps", float("nan")) for r in t["seconds"]], color="#1463ad", alpha=.28)
ax.plot([i+.5 for i in range(30)], [statistics.median(t["seconds"][i]["fps"] for t in light) for i in range(30)], color="#1463ad", label="3 native trials: median")
ax.axhline(statistics.mean(t["windows"]["baseline"]["fps"] for t in light), color="gray", ls="--", label="Before release: 54.14 FPS")
ax.axvspan(8, 12, color="#e9aa3f", alpha=.2)
ax.set(title="Native gameplay: repeatable late-stage slowdown", ylabel="Guest flip FPS", xlabel="Seconds after release (native)")
ax.legend(loc="upper right"); ax.set_ylim(20, 60)
ax = axes[1]
for j, key in enumerate(("draws", "dispatches")):
    for i, t in enumerate(t for t in native["trials"] if t["profile"] == "counters" and t["status"] == "collected"):
        points = sorted(t["snapshots"], key=lambda p:p["seconds"])
        ax.plot([p["seconds"] for p in points], [p[key] for p in points], color=("#1463ad", "#b65025")[j], alpha=.5,
                label=("Draws/frame", "Dispatches/frame")[j] if i==0 else None)
ax.set_yscale("log"); ax.axvspan(8, 12, color="#e9aa3f", alpha=.2)
ax.set(title="Native workload: draw surge followed by compute surge", xlabel="Seconds after release (native)", ylabel="Commands per frame (log)", xlim=(-10, 30))
ax.legend(loc="upper right")
ax = axes[2]
t = next(t for t in full["trials"] if t["status"] == "collected")
pts = sorted([p for p in t["snapshots"] if 0<=p["seconds"]<=110], key=lambda p:p["seconds"])
ax.plot([p["seconds"] for p in pts], [p["dispatches"] for p in pts], color="#b65025", label="Dispatches/frame in Full")
s = ROOT / full["session"]
q = json.loads((s / "diagnostics/data-quality.json").read_text())
capture = q["checks"]["nsight"]["sdk_start_unix_s"]-t["release_ns"]/1e9
ax.axvline(capture, color="#1463ad", ls="--", label=f"Nsight detail start: {capture:.1f}s")
ax.axvspan(25, 42, color="#e9aa3f", alpha=.2, label="CPU peak-analysis window")
ax.set(title="Full instrumentation: detail GPU capture occurs after dispatch peak", xlabel="Seconds after release (Full; different simulation cadence)", ylabel="Dispatches per frame", xlim=(0, 110))
ax.legend(loc="upper right")
for ax in axes:
    ax.grid(alpha=.18)
fig.savefig(OUT / "timeline.png", dpi=150)
plt.close(fig)
