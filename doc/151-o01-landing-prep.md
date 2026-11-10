# 151 — O01 落地预备（默认关接线 + 验收入口）

- 日期（Asia/Shanghai）：2026-11-10 00:16+08
- 作者：开发-反馈边界 / O01
- 依据：`doc/144`、`doc/147`、当前树 `BeginReadback` / Prefetch Flush 位点；PM：无 `O01_candidate` 前不改完成边界；常规验收优先 `diagnostics\super_feedback.bat`（测试 `doc/150`）；裸 `super.bat` 不得当 off；跳过占用预检；禁构建叠采、禁 Full
- 状态：**默认关接线已落在工作树**；未改完成边界、未构建、未采集、未提交（与仪器在飞 src 同文件）

---

## 0. 结论先行

**现在能先做（不改变现行 Flush 语义）：** 预留默认关开关与验收命令，写清触点/风险/回执。脚本与 `perf_tuning` 的「关=零行为」接线可在授权后先于补丁落地（候选名见下），不碰完成边界逻辑。

**现在不能做：** 改 `BeginReadback`/`Prefetch` 的 `Flush` 时机；构建；任何采集（含 Full、叠采）。仪器 S1 仍统一 `missing_gpu_end`，**无** `O01_candidate`。

**卡住：** 等 M2-A 补 GPU end、后段样本覆盖且标签稳定指向 O01。工作树已有大量与本任务无关的脏文件；一旦动代码建议独立分支 `feat/o01-early-complete`，避免与 develop 互踩。

---

## 1. 改点定位（当前树，以符号为准）

| 优先级 | 位点 | 现行行为 | O01 关系 |
|---|---|---|---|
| **主** | `BufferCache::BeginReadback`（`buffer_cache.cpp` ~1667–1671） | 非 queue：`WaitComputeWriter` + `tick=CurrentTick()` + `MarkReadbackSession` + **`scheduler.Flush()`** | copy/publish 拴在**整段当前录制会话**；producer 已可完成时仍被批尾拖住 |
| 同构 | `BeginRelaxedReadback` ~662–666 | 同样的非 queue Flush | 无人等待的放松读；**不是**主验收路径，补丁勿顺手改语义 |
| 相关、先不动 | `PrefetchReadbacksImpl` 末尾 ~1879–1881 | 本批有 graphics 预取则一次 `Flush`，计数 `ReadbackPrefetchFlushes` | 更近 O08/批合并；O01 单分支禁止顺手改预取策略 |
| 辅 | `Scheduler::{Flush,SubmitExecution,MarkReadbackSession}` | Flush=提交新 tick、开新 session | 提供「有限完成边界」，禁止每 dispatch Flush |
| 已提交理想路径 | `use_queue && writers.submitted` → `readback_queue->Submit` | 只等写者 waits | **保持**；O01 不改此路径 |

资格（拟，标签确认后由 Opus 5.5 选型）：仅 **精确 demand / 已学习计数页**，且可证明 producer 已结束或已 submitted 可独立完成；否则保持现状 Flush。保留 submitted gate、版本、重叠 copy、保护规则。

---

## 2. 默认关安全路径（建议候选名）

候选名：**`early-complete`**（已进入 `ValidateSet` / `Get-SuperFeedbackVariant`）。

建议调谐键：`readback_early_complete`，默认 **`false` / `0`**。

接线清单（授权后可先于边界逻辑落地，关时必须与今天行为逐条相同）：

1. `Common::Perf::Tuning`：`std::atomic<bool> readback_early_complete{false}`
2. `perf_sampler.cpp`：默认重置为 false；解析 ini 键
3. `benchmark_control.cpp`：回执字段（开/关 + 建议计数：定点提交次数 / 回退 Flush 次数 / 额外 submit）
4. `Get-SuperFeedbackVariant`：B 置 `readback_early_complete=1`，A/`-CandidateBaseline` 置 0；其它实验键保持与现有候选相同的关闭基线
5. `collect_super.ps1` / `collect_feedback_pair.ps1` 的 `ValidateSet` 加入 `early-complete`
6. 普通 `run.bat` **不**打开该键；仅诊断入口显式 `-Candidate early-complete`

开关关闭时：不得走进新分支，不得改变 Flush 次数/队列选择。这是安全门，不是性能补丁。

---

## 3. 验收入口（跟测试 `doc/150`）

测试已占用 `doc/150-o01-super-bat-acceptance-mapping.md`。O01 性能对照优先：

```text
diagnostics\super_feedback.bat -Candidate early-complete -Mode pro4k -Profile lightweight -Order BA
diagnostics\super_feedback.bat -Candidate early-complete -Mode pro4k -Profile lightweight -Order AB
```

`-CandidateBaseline` 为 off（A），省略则为 on（B）。仪器关。禁 Full。跳过占用预检。

**不要用裸 `diagnostics\super.bat` 当 off**：未指定 `-Candidate` / `-Experiment` 时，它会默认 `compute-barriers`，不是 O01 基线。

对照口径：peak / late / P99 / >50ms，与战役 50+ 目标差异；方向好但未达 50+ → 有限保留。正确性与额外 submit 成本见 `doc/147`。

## 4. 风险

- 额外 Flush/submit 增加 CB 成本，吃掉与后续 GPU 工作的重叠
- 误把 producer 内核时间当批尾 → 拆批无效或变差（应转 O03）
- 资格过宽：把无关 draw/dispatch 切进完成边界
- 顺手改 Prefetch Flush / 池 / O02 足迹 → 单分支违规
- 每 dispatch Flush：直接拒绝
- 开关泄漏进默认 `run.bat`
- 脏树冲突：在 develop 直接改易与仪器/其它候选缠在一起

停止条件同 `doc/142` §6 与 `doc/147`。

---

## 5. 与仪器/验收的依赖

- 仪器：develop `4d8816d` / `0d9f8a9`，默认关；S1 暂统一 `missing_gpu_end`，**不会**出现 `O01_candidate`
- 验收门：`doc/147`；当前状态仍是 **前置未齐**
- 基线锚点：Full `state/super-captures/20261109-224202-pro4k-9cc19a43`；super_cpu `…20261109-225911-pro4k-f55a8933`（对照用，非 O01 治疗）

---

## Decision

默认关接线已完成（关时与现行 Flush 语义相同）。下一步：等稳定 `O01_candidate` 后，用 Opus 5.5 做 `BeginReadback` 最小定点边界；验收走 §3 `super_feedback.bat`（见测试 doc/150）；禁把裸 super.bat 当 off。

*End of doc/151.*
