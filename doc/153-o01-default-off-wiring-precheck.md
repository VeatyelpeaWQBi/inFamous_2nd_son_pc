# 153 — O01 默认关开关接线预检（本轮收工）

- 日期（Asia/Shanghai）：2026-11-10 00:24+08
- 作者角色：测试-反馈验收（结对：开发-反馈边界）
- 性质：只读预检；**不构建、不采集、不改 `src`**
- 依据：`doc/147`、`doc/150`、`doc/151`；PM：仅批默认关接线，不改 Flush、避让 M2-A；用户本轮收工
- 对照编号：`doc/150` 映射保留；landing prep 已改 `doc/151`；O02 开关验收为 `doc/152`

---

## 判定

| 项 | 结论 |
|---|---|
| **默认关接线预检** | **通过（有限）** |
| **O01 性能/结果验收** | **前置未齐**（无 `O01_candidate`、无完成边界补丁、无授权 A/B） |
| 本轮 | **停**；不再开下一轮、不采集 |

「有限」含义：开关与诊断入口已接通，且关时不改变现行 Flush；引擎**尚未消费**该开关，因此 ON 也不会改变完成边界——这符合当前授权，但**不能**当作性能通过。

---

## 1. 已核对（工作树，只读）

### 1.1 调谐默认关

- `Common::Perf::Tuning::readback_early_complete` 初值为 `false`（`perf_stats.h`）
- `perf_sampler.cpp` 重置路径写回 `false`，并解析 ini 键 `readback_early_complete`
- `benchmark_control.cpp` 回执：`readback_early_complete`、`feedback_early_complete_submits`、`feedback_early_complete_fallbacks`
- 计数器仅声明/上报，工作树内**无**递增点（保持 0 符合「未改边界」）

### 1.2 诊断入口（`doc/150` 对齐）

- 候选名 `early-complete` 已进入 `collect_super.ps1` / `collect_feedback_pair.ps1` 的 `ValidateSet` 与 `Get-SuperFeedbackVariant`
- 变体：A / `-CandidateBaseline` → `readback_early_complete=0`；B → `=1`；默认 tuning 哈希里该键为 0
- 普通 `run.bat` **不**提及、不打开该键（仅有 `--super-pool` 等既有入口）

预备命令（**本轮不跑**）：

```text
diagnostics\super_feedback.bat -Candidate early-complete -Mode pro4k -Profile lightweight -Order BA -ObserveEnvironment
diagnostics\super_feedback.bat -Candidate early-complete -Mode pro4k -Profile lightweight -Order AB -ObserveEnvironment
```

注意：`doc/151` §3 示例省略了 `-ObserveEnvironment`；正式性能场仍以 `doc/147`/`doc/150` 为准，须带该开关。裸 `super.bat` 仍不得当 off 基线。

### 1.3 未改 Flush（符合授权）

`buffer_cache.cpp` 非 queue 路径仍是无条件 `WaitComputeWriter` + `MarkReadbackSession` + `scheduler.Flush()`：

- `BeginReadback` ~1668–1671（主路径）
- `BeginRelaxedReadback` ~663–666
- `PrefetchReadbacksImpl` ~1880–1881（本预检确认未顺手改；O01 单分支本就禁止改预取）

`src/video_core` 内**没有**读取 `readback_early_complete` 的分支。

---

## 2. 明确未做 / 未过

- 无稳定 `O01_candidate`，定点完成边界未实现
- 未构建、无新 EXE、无 off/on 轻量 A/B，故 §2 等待缩短 / §3 热点 / §4 正确性（运行时）无法勾
- 开仪器场仍不当 FPS 收益；不要求 Full；本轮不采集

---

## 3. 醒来后的下一动作（不在本轮执行）

1. 等 M2-A 补 GPU end 且标签稳定为 `O01_candidate`
2. 授权后由开发用 Opus 5.5 做 `BeginReadback` 最小定点边界（禁每 dispatch Flush，不改池/O02）
3. 向 PM 预约互斥时段，用上面的 `super_feedback.bat` BA→AB 做结果验收

*End of doc/153.*