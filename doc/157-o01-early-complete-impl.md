# 157 — O01 early-complete 完成边界实现（默认关）

- 日期（Asia/Shanghai）：2026-11-10 12:40+08
- 作者：开发-反馈边界 / O01
- 分支：`feat/o01-early-complete-impl`（从 `develop` @ `eb52d90` 另开工作树）
- 状态：完成边界逻辑已实现；**开关默认关**；未打开、未构建、未采集

## 行为

`readback_early_complete=0`（默认）：`BeginReadback` 与今日完全相同，不走进新分支、不改 Flush 次数或队列选择。

开关打开且资格满足时（精确模式、非写、需求页大小 ≤ 一页），非 queue 路径不再把 copy 录进当前 graphics 会话再 `Flush`（wait 拴在整段批尾）。改为：

1. 若写者已 submitted，或已 Flush 给录制线程但尚未交驱动：先 `WaitSubmitted`（禁止对未提交 tick 走 readback queue，以免冻结），再将 copy 交给 `readback_queue`（只等写者 waits）。
2. 若 queue 未启用：先提交当前已有工作（若 `HasWork`），再把 copy 录进**新的** graphics 会话并 `Flush`，guest 等的是该 copy tick。
3. 写者仍在**正在录制**的会话里（`writers.current`）：无法与已录批尾拆开，回退现行 `WaitComputeWriter` + `MarkReadbackSession` + `Flush`，计数 `feedback_early_complete_fallbacks`。

已 submitted 且 queue 可用的理想路径不变。不改 `BeginRelaxedReadback`、不改 Prefetch Flush、不做每 dispatch Flush。普通 `run.bat` 不打开该键。

## 计数

- `ReadbackEarlyCompleteSubmits` / 回执 `feedback_early_complete_submits`：走了定点提交
- `ReadbackEarlyCompleteFallbacks` / 回执 `feedback_early_complete_fallbacks`：资格到了但回退现行 Flush

## 仍由开关挡住

逻辑已进源码，但默认关时计数保持 0、完成边界不变。验收入口仍跟测试 `doc/150`：`diagnostics\super_feedback.bat -Candidate early-complete`。不要把裸 `super.bat` 当 off。

*End of doc/157.*
