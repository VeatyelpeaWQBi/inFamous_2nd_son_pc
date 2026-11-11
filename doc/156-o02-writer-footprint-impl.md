# 156 — O02 精确写者足迹 Draft A（默认关）

- 日期（Asia/Shanghai）：2026-11-10 13:20+08
- 修订（Asia/Shanghai）：2026-11-11 12:45+08 — 三个路径计数写入 status JSON，并从 perf_log 门解绑
- 角色：开发-写者足迹 / O02
- 工作树：`D:\projects\inFamous_2nd_son_pc-wt-o02`，分支 `feat/o02-writer-footprint-impl`（自 `develop@eb52d90`）
- 真源：`doc/148` §3 草案 A；`doc/152` / `doc/146` 验收语义
- 范围：**仅 Draft A（登记侧收窄）**，全部语义在 `readback_writer_footprint` 默认 **关** 之后。不采集、不构建游戏、不打开开关。

---

## 1. 做了什么

当且仅当调谐键为 1，且编译期能证明该 Guest buffer 的每一次 store/atomic 都是「无 VGPR index / 无 voffset」的常量化字节偏移（立即数，或 `flattened_ud_buf` 中的 soffset）时：

- `SetWriteTick`、`gpu_modified_ranges`、写侧 `NoteQueueAccess`、以及 `InvalidateMemoryFromGPU` / `ClearSkippedWrites` 使用该证明子范围；
- `VkDescriptorBufferInfo` / `buffer_infos` **仍绑定完整 V# 大小**；
- 绑定范围仍按完整 descriptor 做上传/常驻。

资格失败（动态 index、voffset、add_tid、swizzle、无法解析的 soffset、偏移越界等）立即回退完整 descriptor，与今日粗路径相同。

## 2. 明确未做

- Draft B（`FeedbackWriteTicks` 等待侧精化）。干净 `develop` 上不存在未入库的 `FeedbackPageWriters` / `FeedbackWordWriters`；本补丁不把脏树里的 `feedback-writers` 实验搬进来。
- 不收窄 GDS（仍按整个 GDS buffer 记账；没有独立资格证明不收窄）。
- 不改 `BYTES_PER_WORD`、不硬编码 VA、不取消写者跟踪、不混 O01 Flush / prefetch / 池布局。
- `run.bat` 仍不打开该键。`collect_super.ps1` / `Get-SuperFeedbackVariant` 等脚本在 `develop` 上从未入库，本分支不搬运脏树未跟踪脚本套件。

## 3. 开关

- 键：`readback_writer_footprint`，`ApplyGameProfile` 重置为 false，INI 可开。
- 键为 0 时：`BindBuffers` 不计算足迹，`ObtainBuffer` 不走新分支，行为应与今日位级一致。
- 键为 1 时：上述证明路径才会改写登记与别名失效范围；计数 `WriterFootprintProven` / `Fallbacks` / `Narrowed`。

这三个计数从 `perf_log` 门解绑，走进程级原子量，并进入 status 回执：`writer_footprint_proven` / `writer_footprint_fallbacks` / `writer_footprint_narrowed`。轻量采集（`perf_log=0`）也会在回执里看到实际路径次数，不必开 `-Profile counters`。其它 Perf 计数仍受 `perf_log` 门控。

- 着色器缓存：`ShaderMetaVersion` 6→7（`BufferResource` 增加足迹字段）。

## 4. 验证

- 纯函数测试：`ifss_writer_footprint_test`（`tests/test_writer_footprint.cpp`），不启动游戏。
- 开关对照采集仍按 `doc/152`，须 PM 授权与互斥时段；本轮未跑。

*End of doc/156.*
