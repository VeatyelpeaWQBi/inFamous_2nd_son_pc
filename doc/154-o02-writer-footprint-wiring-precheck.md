# 154 — O02 `writer-footprint` 默认关接线只读预检

- 日期（Asia/Shanghai）：2026-11-10 00:25+08
- 角色：测试-足迹验收（结对：开发-写者足迹）
- 对照门槛：`doc/146` + `doc/152`（PM 已采纳）
- 开发说明：`doc/148` §6
- 范围：**只读预检**接线是否符合「仅默认关 `writer-footprint`、不改足迹逻辑」。**不构建、不采集、不开下一轮。**

---

## 判定

| 项 | 结论 |
|---|---|
| 接线预检 | **通过**（只读） |
| O02 验收（假依赖/效果） | **证据不足 / 暂缓**（保持） |
| 采集 | **未做**；本轮到此停止 |

接线通过 ≠ O02 通过。尚无 `O02_candidate`、无 Draft A/B 足迹语义、无前后 late FPS / tick 表。

---

## 核对结果（对照 `doc/146` / `doc/152`）

| 检查项 | 结果 |
|---|---|
| 新候选名 `writer-footprint` 进入 ValidateSet | 是：`scripts/collect_super.ps1`、`collect_feedback_pair.ps1`、`super_capture_tools.ps1` |
| 新键、默认关 | 是：`readback_writer_footprint`；`Get-SuperFeedbackVariant` 初值 0；`Perf::Tuning` 默认 `false` |
| OFF/ON | A（`-CandidateBaseline`）→ 0；B → 1。不置 `readback_feedback_writers` 等旧键 |
| 禁止旧 Candidate 冒充 | 分支独立；注释写明不启用 `feedback-writers` / Draft A/B 逻辑 |
| 普通 `run.bat` 不被打开 | `run.bat` 无此键。`ApplyGameProfile`（`perf_sampler.cpp` ~L78）将其置 `false`；随后 `emulator.cpp` 先 profile、再 `PollTuning()`，诊断 INI 才能打开 |
| 不改足迹逻辑 | `src` 中该原子仅出现于 `perf_stats.h` 声明、`perf_sampler.cpp` 重置/INI 解析、`benchmark_control.cpp` 回执。`SetWriteTick` / `BindBuffers` / `FeedbackWriteTicks` / `buffer_cache` **无引用** |
| 未混 O01 拆批 / 池布局 | `writer-footprint` 分支只改这一键。同脚本另有 `early-complete`（O01），键分离，不纳入本预检否决 |
| 主入口 | 仍为 `diagnostics\super_feedback.bat -Candidate writer-footprint`（先 BA 再 AB，`-ObserveEnvironment`）；单场可用 `super.bat` 同参数。`Get-SuperStartupArguments` 无额外 EXE 旗标，走 INI tuning（与历史 INI 型候选相同） |

工作树里 `buffer_cache` / `vk_rasterizer` 仍有既有脏文件，但 **不含** `readback_writer_footprint`，不记为本接线违规。

---

## 未做 / 明确停止

- 未跑 `super_feedback.bat` / `super.bat` / Full，未申请互斥时段。
- 未构建；效果门与机制前后表仍缺。
- 下一轮（足迹逻辑 / `O02_candidate` 后再验）**不在本轮开启**。

*End of O02 wiring precheck. 测试-足迹验收*

