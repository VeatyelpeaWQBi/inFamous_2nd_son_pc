# 150 — O01 验收：super.bat 开关对照与预备判据

- 日期（Asia/Shanghai）：2026-11-10 00:15+08
- 作者角色：测试-反馈验收（结对：开发-反馈边界）
- 状态：按 `doc/147` 写清入口/开关对照与预备判据；**只写 `doc/`**；不改 `src`、不采集、不构建
- 依据：`doc/141`、`doc/142`、`doc/144`、`doc/147`、`doc/149`；`diagnostics/super*.bat` 与 `scripts/collect_super.ps1` / `collect_feedback_pair.ps1` 现参
- 前提：O01 最小补丁**尚未落地**；本文件先锁定「怎么验」，不宣称结果

---

## 0. 核心目标（对 147 的执行翻译）

O01 要通过，必须同时证明：

1. **等待缩短**：后段窗口里，producer 已结束后，copy/publish 不再被同批尾部拖住（关联时间线或等价 tick）
2. **热点 / 交换**：仪器关闭的性能场上，反馈串行等待占比下降或 CPU↔GPU 重叠改善；无「每帧过度 Flush」新热点
3. **正确性**：submitted gate、版本、代次、重叠 copy、保护、反馈不丢、图像过；无降画质换帧

战役 50+ FPS 仍是方向，**单补丁未达 50+ 最多「有限保留」，不宣称 Done**。

**不要求每次 Full。** Full / VTune 峰值 FPS **不作**通过条件，仅在需要 GPU 时间线时另约一场。

---

## 1. 入口对照（勿用错 bat）

| 入口 | 实际调用 | 默认行为（易踩坑） | 对 `doc/147` | 是否每次要 Full |
|---|---|---|---|---|
| `diagnostics\super.bat` | `scripts\collect_super.ps1 %*` | 未显式 `-Experiment` 且 `-Candidate none` 的轻量/suite **会默认 `compute-barriers`**；**不能**当干净 off 基线 | 仅当显式 `-Experiment none` 时可用于关仪器烟雾或辅助轻量场 | 否 |
| `diagnostics\super_feedback.bat` | `scripts\collect_feedback_pair.ps1`（已固定 `-Experiment none`、Rounds 1×2 进程） | 需带 `-Candidate`、`-Order BA\|AB`、`-ObserveEnvironment` | **§1/§2 性能 A/B 主入口**（O01 补丁有候选名之后） | 否 |
| `diagnostics\super_feedback_link.bat` | **只打印说明，不启动游戏、不采集** | 文档：开仪器写 `feedback_link_sample=1`；导出 `state\sessions\<id>\diagnostics\feedback-link-samples.jsonl` | 分流/仪器场说明书；**不当 FPS 验收** | 否 |
| `diagnostics\super_cpu.bat` | `collect_super.ps1 -CpuHotspots -Profile lightweight -Rounds 1 -Experiment none` | 禁止与 Full/Critical/候选同场 | **§3 热点辅助**，单独一场；不作通过条件 | 否（且禁与 Full 并行） |
| `diagnostics\super_full.bat` | `collect_super.ps1 -FullCapture` | 显著拖慢；FPS 不代表游玩 | 仅可选时间线；**不要求、不作通过** | 仅此入口，按需另约 |
| 普通 `run.bat` / `--super-pool` | 游玩/池实验 | **不得**隐式打开 `feedback_link_sample` | 不用作正式 O01 A/B | 否 |

互斥（已按 PM 修订）：启动前**不**查硬件占用、**不**让用户清桌面或监控；直接开跑。互斥**仅**指本机不能两个采集/游戏进程重叠。与其它测试 bot 抢窗口时先向 PM 申请时段。

---

## 2. Off / On 矩阵

### 2.1 仪器（M2-A，`doc/149`）— 与 O01 补丁分场

| 状态 | 如何开/关 | 核心目标 | 后段 FPS 判据 | Full |
|---|---|---|---|---|
| **OFF** | `feedback_link_sample=0`（默认），未设 `IFSS_FEEDBACK_LINK_ARMED`；普通 `super.bat`/`run.bat` 不得打开 | 证明热路径无泄漏 | 同 EXE、同分辨率/存档/视角，后段相对安静基线 `abs(delta_pct) ≤ 2.0`；环境不合格则重跑，不否决 | 不要求 |
| **ON** | 会话 `perf_tuning.ini`：`feedback_link_sample=1`；可选 `set IFSS_FEEDBACK_LINK_ARMED=1`；再**另开**已授权的 Critical/轻量+trace 采集（本 bat 本身不跑） | 产出 `FeedbackLinkSample` 并打标签；稳定 `O01_candidate` 才准入补丁验收 | **不做 FPS 验收**（开仪器场数字不作通过/驳回依据） | 不要求；禁止用 Full FPS 当分流证据 |

`delta_pct = (smoke_off_late_fps - baseline_late_fps) / baseline_late_fps * 100`（定义同 `doc/149` §10.1）。

缺 `gpu_end` 时禁止只靠 `ReadbackProducer*` 粗计数打成 `O01_candidate`（`doc/149` §10.2）。

### 2.2 O01 补丁（M2-B）— 开关名待开发落盘后填

补丁未写、无新 EXE。预备约定：

| 状态 | 预备开法（补丁落地后按实际候选名替换 `<o01-candidate>`） | 核心目标 | 后段 FPS 判据 | Full |
|---|---|---|---|---|
| **OFF** | 同构建、开关关；pair 的 baseline 侧（`-CandidateBaseline`）或未启用定点完成边界 | 对照基线 | 记录 late/peak、P99、>50ms | 不要求 |
| **ON** | 同构建、开关开；pair 的 treatment 侧；**禁止**每 dispatch Flush；**不**改池/O02 | 缩短 producer end → copy/publish；等待下降 | 见 §3；仪器必须关 | 不要求 |

正式性能命令模板（补丁有候选名且已授权、已约互斥时段后再跑；**现在不跑**）：

```text
diagnostics\super_feedback.bat -Candidate <o01-candidate> -Mode pro4k -Profile lightweight -Order BA -ObserveEnvironment
diagnostics\super_feedback.bat -Candidate <o01-candidate> -Mode pro4k -Profile lightweight -Order AB -ObserveEnvironment
```

关仪器烟雾（仪器合并并构建后；显式禁止默认 experiment）：

```text
diagnostics\super.bat -Mode pro4k -Profile lightweight -Rounds 2 -Experiment none -ObserveEnvironment
```

热点辅助（可选、分场、不作通过条件）：

```text
diagnostics\super_cpu.bat
```

---

## 3. 后段 FPS 与等待（预备通过线）

窗口与 `doc/141` 一致：报告 baseline / early / **peak** / **late** / recovery / tail + **P99** / **>50ms**。只看全段均值不算。

| 项 | 预备判据 |
|---|---|
| 主证据 | late 窗口 producer end → copy/publish 间隔缩短（jsonl / tick 时间线） |
| FPS | 仪器关的轻量 pair：late 或 peak **方向改善**，或 FPS 持平但等待证据明确改善 |
| 硬否决 | 主要耗时仍在 producer 内核 → 转 O03，不通过；额外 submit/CB 吃掉重叠 → 不通过 |
| 有限保留 | 方向对但远未 50+，或样本不足需再一轮 |
| 禁用 | 用 Full FPS、开仪器场 FPS、跨 EXE、环境不合格场来定通过 |

当前对照基线（非 O01 治疗，仅预备引用）：

- Full 批次 `state/super-captures/20261109-224202-pro4k-9cc19a43` → session `…9d82ca07-diagnostic-full`（辅助时间线，非 FPS 门槛）
- super_cpu 批次 `state/super-captures/20261109-225911-pro4k-f55a8933` → session `…1cea726f`
- 紧凑池历史 `doc/super-feedback-compact-*-20261108.json`（池候选，**不是** O01 off/on）
- 当前 receipt EXE `runtime_sha256=6d9d4fbdb52e5490e9fbfe168bf0df8e7af5a20132cbceda57c404e470246205`（补丁后必须重核指纹）

---

## 4. 与 147 章节映射（验收时逐项勾）

| 147 节 | 用哪场、哪个开关 | 预备结论 |
|---|---|---|
| §0 互斥 | 任意一场采集/游戏；不并行第二进程 | 规则已写清；开跑前不查占用、不清桌面 |
| §1 准入 | 仪器 ON 场产出稳定 `O01_candidate` + 随后补丁 + 仪器 OFF 的 pair | **当前前置未齐**（无标签、无补丁） |
| §2 等待缩短 | 仪器 OFF 的 `super_feedback.bat` off/on pair + 关联时间线 | 补丁未到，判据已锁定 |
| §3 热点/交换 | 可选 `super_cpu.bat` 分场；禁止用 Full 峰值定案 | 不要求每次都采 |
| §4 正确性 | 同上性能场的图像/机制回执；破坏则立即驳回 | 不因无补丁而放宽 |
| §5 判定 | 通过 / 有限保留 / 驳回 / 前置未齐 / 环境失败 | 现在只能是 **前置未齐** |
| §7 doc/144 | 仅「可定点完成」路径改完成边界；否则保持 Flush | 等补丁 diff 再勾 |

---

## 5. 明确不做

- 空等补丁而不把判据写清（本文件已完成）
- 未授权采集、把池 pair 或今晚 Full/hotspots 当作 O01 治疗对照
- 裸跑 `super.bat`（未带 `-Experiment none`）当 off 基线
- 每次验收都跑 Full
- 测试侧改 `src`

*End of doc/150.*