# 148 — O02 精确写者足迹只读预备（代码落点 + 方案草案 + 风险）

- 日期（Asia/Shanghai）：2026-11-09 23:40+08；2026-11-10 00:25+08 补 §6 默认关开关接线
- 角色：开发-写者足迹 / O02
- 真源：`doc/64` O02；`doc/140` P2；`doc/142`；`doc/146`
- 范围：只读预备 + §6 PM授权默认关开关。无 `O02_candidate` 前不做 Draft A/B。
- 工作方式：本机工作树；不强制 PR。

---

## 1. 假依赖机制（256 KiB word）

`SetWriteTick` 按 word 下标抬高整段 256 KiB wait frontier。descriptor 大范围登记 + 粗粒度 tick = 反馈短读可能等待同 word 内未触及子范围的后期写者。

---

## 2. 代码落点

- 写：`BindBuffers` / `ObtainBuffer` / `SetWriteTick` / GDS
- 读：`BeginReadback` / `FeedbackWriteTicks` / `WindowWritersOf`
- 旁路：`FeedbackPageWriters` / `FeedbackWordWriters` / `SelectReadbackDependencies`

---

## 3. 草案（待 O02_candidate）

A 登记侧收窄；B 等待侧精化；C 禁止取消全部写者/硬编码 VA/混 O01。

---

## 4. 风险

资格过宽/漏登记→正确性否决；别名/GDS/Capacity/仪器/与 O01 抢等待均回退与分流隔离。

---

## 5. 状态

- 判定：证据不足 / 暂缓
- 下一动作：等稳定 `O02_candidate` 再做 Draft A/B

---

## 6. 默认关开关接线（2026-11-10，PM 授权「仅默认关接线」）

- 候选名：`writer-footprint`（**不是** `feedback-writers`）
- 调谐键：`readback_writer_footprint`，默认 **`false` / `0`**
- 已接线：`perf_stats.h` Tuning；`perf_sampler.cpp` 重置/INI；`benchmark_control` 回执开/关；`ValidateSet` + `Get-SuperFeedbackVariant`（B=1，A/`-CandidateBaseline`=0）
- `run.bat` **不**打开该键
- **仍未做：** Draft A/B、`SetWriteTick` 收窄、`FeedbackWriteTicks` 精化、`BindBuffers` 写路径语义
- 验收：`doc/152`（勿占 `doc/150`）

*End of doc/148.*
