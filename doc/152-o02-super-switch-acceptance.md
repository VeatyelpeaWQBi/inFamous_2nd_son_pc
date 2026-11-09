# 152 — O02 `super.bat` 开关对照验收判据（预备）

- 日期（Asia/Shanghai）：2026-11-10 00:16+08（2026-11-10 00:19+08 由 `150-o02-super-switch-acceptance.md` 改号为 152；`doc/150` 留给 O01，本文不改 150）
- 角色：测试-足迹验收（结对：开发-写者足迹）
- 验收门（正式通过门槛）：`doc/146`
- 只读预备真源：`doc/148-o02-footprint-prep.md`
- 范围：把 **off/on 开关、后段 FPS、足迹/假依赖目标** 写成可执行对照；**不要求每次 Full**。补丁未到也可按此预备。
- 状态：**默认关开关已接线**（候选 `writer-footprint` / 键 `readback_writer_footprint`=0）；**仍不授权、不触发采集/构建**；Draft A/B 等 `O02_candidate`。勿占 `doc/150`。

---

## 0. 流程硬规则（与 `doc/146` 一致，此处写进执行对照）

1. 本机同一时刻只允许 **一场** 采集或游戏进程（含 lightweight pair、Critical、Full、VTune、Nsight）。冲突先向 PM 申请时段。
2. 互斥 **仅** 指两个采集/游戏进程不可重叠。启动前 **不查硬件占用**，不请用户清桌面或监控；获授权与时段后直接跑。
3. 环境不合格试验不得记为候选否决（`doc/139`）。
4. 测试侧不擅自改 `src/`。

---

## 1. 入口与 off/on 语义

`diagnostics\super.bat` 把参数原样交给 `scripts\collect_super.ps1`（单场释放）。
成对 A/B 必须走 `diagnostics\super_feedback.bat` → `scripts\collect_feedback_pair.ps1`（新鲜进程、两侧各一轮；脚本本身就不跑 Full）。

| 侧 | 含义 | 开关 |
|---|---|---|
| **OFF（A）** | 粗 256 KiB word / 完整 descriptor 写者路径（现有保守真源） | `-Candidate writer-footprint -CandidateBaseline` |
| **ON（B）** | O02 精化足迹打开 | `-Candidate writer-footprint`（不要加 `-CandidateBaseline`） |

两侧固定：`-Mode pro4k -Profile lightweight -ObserveEnvironment`，`-Experiment none`，候选试验 `Rounds=1`（由 pair 脚本保证）。

**候选名：`writer-footprint`。** 已进入 `collect_super.ps1` / `collect_feedback_pair.ps1` / `super_capture_tools.ps1` ValidateSet，映射新键 `readback_writer_footprint`（默认关）。现有 `feedback-writers` / `feedback-word` / `feedback-page-words` / `feedback-word-prefetch` / `feedback-word-bulk` 是历史实验，**禁止**拿来冒充本 O02 验收。开发已完成 / 须保持：

1. 把新名字写入 `collect_super.ps1`、`collect_feedback_pair.ps1`、`super_capture_tools.ps1` 的 ValidateSet；
2. 映射到 **新的、默认关闭** 的 tuning 键（ON=1 / OFF=0）；普通 `run.bat` 保持关；
3. 不得复用上述历史 Candidate 或其旧 INI 键当 O02 主开关。

若开发最终采用其它 **新** 名字，验收语义不变（A=Baseline 关，B=开），本文与判定记录改用实名即可。

---

## 2. 场次拆分（Full 不是每次门槛）

| 场次 | 入口 | 看什么 | 不看什么 |
|---|---|---|---|
| **轻量 pair（效果+机制回执）** | `super_feedback.bat` off/on，建议先 BA 再 AB | 后段 `late` FPS、tail/P99、机制计数 | Full / VTune / Nsight FPS |
| **仪器/Critical（机制样本）** | 另场；如 `super.bat -CriticalCapture` 或 M2-A `feedback_link_sample`（默认关，见 `super_feedback_link.bat`） | 同一反馈窗的 word tick vs 子范围足迹、writer 集合、reductions/fallback | **不当 FPS 验收** |
| **Full** | `super_full.bat` / `-FullCapture` | 仅当 PM 另批互斥时段、且有具体正确性/追踪问题 | **不要求每次**；不得作分流或本门通过条件 |

仪器与性能 pair **分场**，不在同一互斥窗口并行。关仪器时的轻量冒烟：后段相对安静基线漂移应很小（M2-A 文档记 late 窗 ≤2%）；那是仪器开销门，不是 O02 收益门。

---

## 3. 后段 FPS 判据（效果辅证，须授权 + 互斥时段）

报告窗口与 `doc/141` 相同：baseline / early / **peak** / **late（主看后段）** / recovery / tail，外加 P99 与 >50ms 计数。

| 判定 | 预备标准（环境合格的 BA，建议再有 AB 确认） |
|---|---|
| 效果可接受 | late 与 tail/P99 **未双侧一致恶化**；图像审查通过 |
| 效果加分 | late 或 peak 有稳定正方向（记录幅度；**不**因此宣称战役 50+ Done） |
| 效果失败 | 环境合格下 BA 与 AB 均见 late 下降，且 P99 或 >50ms 同时变差 |
| 不自动驳回 | 仅未到 Pro 4K 稳定 50+ FPS |
| 无效数字 | Full/VTune FPS、菜单/快照 FPS、无 `-ObserveEnvironment`、环境不合格窗口 |

效果是辅证：机制无收窄时，即使 FPS 微涨也 **不得** 标通过（见 §4 停止条件）。

---

## 4. 足迹 / 假依赖目标（机制门，可先于 FPS 预备）

对象：压缩 256 KiB word 级假依赖——同 word / 同 descriptor 内 **未触及子范围** 不再抬高 `FeedbackWriteTicks` / wait frontier。对照 `doc/148` 落点（`SetWriteTick`、`BindBuffers`、`FeedbackWriteTicks`）。

ON 相对 OFF，同一反馈需求窗，必须能交出前后表：

1. 粗 word tick / writer 集合 **收窄到可证明的实际触及子范围**（或证明等价）；
2. 能对应资源身份/代次、binding、UD 偏移、producer 与反馈地址；**禁止**硬编码历史 VA；
3. `reductions`（或等价）上升来自剔除未触及子范围，且非「全员 fallback 伪装成功」；
4. hazard/wait frontier 随精化变化；**精化后 frontier 不变 → 停止 O02**（有限保留，不标通过）；
5. 未知动态写 / 资格失败 → 回退完整 descriptor / 粗路径，并有计数。

Vulkan 完整 descriptor 绑定仍可用；精化只作用于已证明的同步信息。

---

## 5. 一张对照表（验收时逐项勾）

| 验收项 | OFF（A） | ON（B） | 通过怎么看 | 每次 Full？ |
|---|---|---|---|---|
| 入口隔离 | 普通 `run.bat` 关 | 仅候选/诊断入口开 | 默认路径不被污染 | 否 |
| 后段 FPS | `late_A`（+ tail/P99） | `late_B`（+ tail/P99） | 见 §3；主看 late | **否，禁止当门** |
| 足迹/假依赖 | 256 KiB word 粗 tick | 子范围精化 tick / 更小 writer 集 | 见 §4；须有前后数字与路径 | 否（机制可另场 Critical） |
| 正确性 | 粗路径为正确性真源 | 失败回退粗路径 | `doc/146` §3 任一破即驳回 | 否 |
| 否决项 | — | 不混 O01/池、不取消全部写者、不复用历史负收益开关 | `doc/146` §4 | 否 |

---

## 6. 授权后命令模板（先向 PM 申请互斥时段；不预检占用、不清桌面）

成对（推荐，先 BA 再 AB）：

```text
diagnostics\super_feedback.bat -Candidate writer-footprint -Mode pro4k -Profile lightweight -Order BA -ObserveEnvironment
diagnostics\super_feedback.bat -Candidate writer-footprint -Mode pro4k -Profile lightweight -Order AB -ObserveEnvironment
```

单场（调试/补跑一侧，不作完整 pair 替代）：

```text
diagnostics\super.bat -Candidate writer-footprint -CandidateBaseline -Mode pro4k -Profile lightweight -ObserveEnvironment
diagnostics\super.bat -Candidate writer-footprint -Mode pro4k -Profile lightweight -ObserveEnvironment
```

不要用 `diagnostics\super_full.bat` 或 `-FullCapture` 作为本门通过条件。

裸 `super.bat`（无 `-Candidate`）轻量默认会进 `compute-barriers`；关仪器烟雾须显式 `-Experiment none`。带 `-Candidate writer-footprint` 时由候选路径避开该默认实验。开关已接线后，上表命令应可解析候选名（仍须 PM 互斥时段才实跑）。

---

## 7. 补丁未到时的预备动作（现在即可）

- 只读预检开发 diff：是否 **新** 候选名 + 默认关 + 未复用历史 writer/word 开关 + 未混改 O01/池布局。
- 机制表头先定好（同一反馈窗：addr/size、粗 tick、精化 tick、writer 数、reductions、fallback、frontier 是否变化）。
- 判定仍按 `doc/146`：通过 / 有限保留 / 驳回 / 环境失败 / **证据不足·暂缓**。
- **当前判定：证据不足 / 暂缓**（开关已接、默认关；仍无稳定 `O02_candidate` 样本与 Draft A/B；无前后 A/B 数字）。

*End of O02 super.bat switch acceptance criteria. 测试-足迹验收*

