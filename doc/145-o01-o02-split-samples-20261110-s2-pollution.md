# 145 — M2-A S2 仪器 off/on 污染对照正式报告

- 日期（Asia/Shanghai）：2026-11-10 00:25+08
- 作者角色：测试-仪器验收（结对：开发-仪器分流）
- 状态：**FAIL**（开仪器污染超标）。本文件是污染门限验收证据，**不是**关联标签/分流通过，也**不是** 50+ 收益证明。
- 判定门限：同构建、`-Experiment none`，只开关 `feedback_link_sample`，后段 late=[8s,12s)（相对大招 release）`|delta_pct| ≤ 2%` 才通过污染项。
- 关联：`doc/149-m2a-feedback-link-instrument-design.md` §10.1 / §10.3，`doc/142`，`doc/150`（入口与 `-Experiment none` 校正）
- 机器可读副本：`doc/campaign/m2-split-20261110-s2-pollution.json`

## 结论

**驳回开仪器采集验收。** `delta_pct = -53.80%`，远超 `|delta| ≤ 2%`。关仪器路径可继续作为本构建对照基线；开仪器当前不可用于关联样本验收。不放行 M2-B。本轮不开、亦不要求 Full。全组按 PM 指示暂停，待用户醒来后再安排降 ON 热开销。

## 构建与开关

| 字段 | 值 |
|---|---|
| 入口 | `diagnostics/super.bat`，轻量 `pro4k`，`observe_environment=false`，非 Full、非 `cpu_hotspots` |
| experiment / candidate | 两侧 `capture.json` 均为 `none` / `none`（`launch.json` 这两字段为空，以 capture 为准） |
| `compute_barrier_scope` | 两侧均为 0 |
| runtime_sha256 | `04d6a25ac7c04632b76778f680823b240a4e4ae0b50768466ede8a8f5504cd97` |
| game_sha256 | `2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab` |
| 源码基准 | develop 钩子 commit `0d9f8a9fd40fbdf9b11300d2cc9a2c741e63d3fd`（`0d9f8a9`） |
| 树状态 | **非纯净 tree**：HEAD 钩子 + 未提交 harness（开发已披露）。本 SHA 只代表该二进制，不能当成干净 `0d9f8a9` 产物 |
| 显示 | 两侧 `status.json` 均为 2560×1600（launch 声明 pro4k 3840×2160） |
| 并行 / Full | 无。OFF 约 00:18:54+08 开跑，ON 约 00:21:07+08 衔接，无重叠采集 |

## 污染对照（后段 FPS）

late 窗口定义为大招 `release.applied_unix_ms` 起的 `[8s, 12s)`，FPS 由该窗口内 `frames.csv` 的 `interval_ns` 均值倒数复算，与开发短报一致。

| 字段 | OFF | ON |
|---|---|---|
| `feedback_link_sample` | 0 | 1（会话内；开发称跑完已恢复 0 并清除 ARMED） |
| session | `state/sessions/20261110-001855-081-pro4k-9e48256c` | `state/sessions/20261110-002108-417-pro4k-a62ff55e` |
| batch | `state/super-captures/20261110-001854-pro4k-4f3d509b` | `state/super-captures/20261110-002107-pro4k-a437dea7` |
| capture status | collected，exit 0，meter 已消耗，release accepted | 同左 |
| late_window | [8s, 12s) after release | 同左 |
| late FPS | **34.888**（n=139） | **16.118**（n=65） |
| jsonl | 不存在（符合默认关） | `state/sessions/20261110-002108-417-pro4k-a62ff55e/diagnostics/feedback-link-samples.jsonl`（36857 bytes，64 条） |

| 字段 | 值 |
|---|---|
| runtime_sha256 | `04d6a25ac7c04632b76778f680823b240a4e4ae0b50768466ede8a8f5504cd97` |
| game_sha256 | `2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab` |
| baseline_batch | `state/super-captures/20261110-001854-pro4k-4f3d509b`（本轮 OFF，非作废批次） |
| late_window | [8s, 12s) after release |
| baseline_late_fps（OFF） | 34.888 |
| treatment_late_fps（ON） | 16.118 |
| delta_pct | **-53.80%**（`(on - off) / off * 100`） |
| pass_abs_le_2pct | **false（FAIL）** |

本轮没有同 SHA 的历史安静基线可把 OFF 再套一次 ≤2%（`super_cpu` f55a8933 的 late p50≈30 仅作量级参考，跨 runtime，不作本项通过条件）。OFF late 34.888 落在历史后段约 30–35 一带，支持「关仪器可作本构建对照基线」，但不把它写成另一项通过。

## 开仪器样本（仅记录，不作为本轮通过项）

路径（写死契约）：

```text
state/sessions/20261110-002108-417-pro4k-a62ff55e/diagnostics/feedback-link-samples.jsonl
```

| label | count |
|---|---|
| O01_candidate | 0 |
| O02_candidate | 0 |
| O03_redirect | 0 |
| O08_candidate | 0 |
| publish_wake | 64 |
| inconclusive | 0 |

64 条均为 `schema=1`、`label=publish_wake`、`missing_gpu_end=true`（flags=16）。按 §10.2，不得仅凭粗计数打成 `O01_candidate`；本批也确实没有 O01 标签。部分样本 `dispatch_id`/`shader_hash` 仍为 0。关联字段与标签直方图**未达到**分流验收，须等污染项通过后再验。

## 作废与不采用场次

- **作废勿用**（裸 `super.bat` 默认 A/B 优化，不能当 off 基线）：batch `state/super-captures/20261110-001548-pro4k-08a4d945`，session `state/sessions/20261110-001548-928-pro4k-02ac5bdb`
- **环境不合格 INCONCLUSIVE**（不记否决，且此后已改为不因占用拦路）：`state/super-captures/20261109-235923-pro4k-04fc50c1`、`state/super-captures/20261110-000146-pro4k-dea32f5b`

## 缺口与下一轮（醒后再安排；本报告写完即停）

1. 开仪器热路径开销过大。优先核查每次 `FinishReadback` 都 `ExportJsonl` 等 ON 侧同步写盘/采样开销，把 `|delta|` 压回 ≤2% 后再对照。
2. 下一轮构建请用干净树，或再次明确披露 harness 范围；不要把未提交 WIP 混进 M2-A 验收 SHA。
3. 污染通过之前，不放行 M2-B，不用本批 jsonl 做 O01/O02 分流结论，不开 Full。

*End of doc/145 S2 pollution report.*
