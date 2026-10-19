# inFAMOUS Second Son PC

基于 shadPS4 / AYOUB1080p GPL 源码的 Windows x86-64 游戏专用引擎，面向《inFAMOUS Second Son》的 1080p 与 Pro 4K 游玩和性能优化。它在 Windows 上运行 PS4 游戏程序，并非取得游戏源码后重写的 PC 移植版，也不是通用 PS4 启动器。

游戏文件由使用者自行提供，项目和发布包均不附带游戏。原始文件保持只读，游戏补丁只作用于运行中的程序，并检查版本、程序指纹和原始字节。

## 分支与优化状态

| 分支 | 用途 | 启动方式 |
| --- | --- | --- |
| `main` | 与开发分支同步的源码基线，包含 V1/V2 和诊断设施；不是未经修改的上游原版 | 构建后运行 `run.bat` |
| `feature/fps-optimization` | 性能研究、V3 候选和对照测试 | `run.bat`、`diagnostics/` 下的专项入口 |
| `release/optimization-v2` | 保留已验收的 V1/V2，移除 V3、采样、调试面板及外部采集器联动，供发布游玩 | 直接打开 `inFAMOUS Second Son.exe`，进入启动菜单 |

V1/V2 已成为引擎固定逻辑，无需开关：

| 优化 | 解决的问题与保留的正确性条件 |
| --- | --- |
| V1：顶点／索引缓冲绑定去重 | 省去同一命令缓冲中完全相同的绑定，保留上传、屏障和状态失效处理 |
| V1：精确回读范围依赖 | 按实际复制范围选择需要等待的 GPU 生产者；未知写入或版本变化时保守回退 |
| V2：必需计算管线优先编译 | 优先处理当前游戏等待的计算管线，减少被后台编译阻塞；保留有界队列、公平调度和全部 dispatch |
| V2：实际 CPU 读页预取 | 根据已发生的 CPU 读取选择预取页，减少无效回读；保留大小、有效期、版本与完成检查 |

精确回读、SDR、完整粒子及着色器／管线缓存继续保留。已核验游戏构建还使用分辨率、光照列表容量、4K 曝光缓冲容量和防冻结补丁。宽松回读会影响粒子及亮度，不能用其帧数代替完整画面的性能。

V3 仍为实验：当前 C1 光照深度归约已确认命中，但尚未证明稳定的整帧收益；A1 绑定更新等候选也不能当作已验收成果。普通 `run.bat` 和 `diagnostics/full.bat` 默认关闭 V3，Release 不包含这些候选。

## 测试平台与运行表现

本机硬件／系统信息于 2026-10-19 核对：

| 项目 | 配置 |
| --- | --- |
| 电脑 | ASUS ROG Strix SCAR 18 G834JYR |
| CPU | Intel Core i9-14900HX，24 核／32 线程 |
| GPU | NVIDIA GeForce RTX 4090 **Laptop GPU，16 GB 显存**；另有 Intel UHD 核显 |
| 内存 | 64 GB，系统可用约 63.6 GiB |
| 系统／显卡驱动 | Windows 11 家庭中文版，Build 26100；NVIDIA 616.56 |

以下为本机 V1/V2 开发构建的**人工游玩观察**，使用港版 CUSA00309 01.07、精确回读、SDR 和完整粒子；4K 为取消棋盘格的 Pro 3840×2160。不同记录的场景、操作和预热状态有所不同：

| 场景 | 观察结果 |
| --- | --- |
| 1080p 常规游玩 | 约 60 FPS |
| V1 阶段的 Pro 4K 常规场景 | 多数 40 FPS 以上，少数约 38–40 FPS；此前约 30–40 FPS |
| V2 预热后的 Pro 4K 常规场景 | 约 50 FPS 以上 |
| V2 移动／异能释放、双方同时释放异能 | 分别约 45 FPS 以上、41 FPS 以上 |
| 开场车祸、大量火焰／烟雾的高压力场景 | 约 30–35 FPS，仍有优化空间 |

这些记录说明用户已感知 V1/V2 的收益，但不是严格同场景 A/B 测试，不能换算成统一提升百分比，也不代表全游戏最低帧率。首次编译着色器或缓存未预热时可能卡顿；Release 包的逐场景表现仍需另行验证。full 的验证层、采样和抓帧会显著拖慢运行，其 FPS 不能代表正常游玩，也不能简单扣除固定开销还原真实帧时间。

## 使用 Release 包

1. 完整解压 `lite-*.zip` 或 `portable-*.zip`，保留 `support/`、`profiles/` 及配置文件。
2. 将完整游戏文件放入包内 `game/`，使其包含 `eboot.bin` 和资源；也可使用 `game/CUSA00309/` 等地区目录，或配置外部游戏路径。
3. 双击 `inFAMOUS Second Son.exe`，在菜单中选择分辨率、显示模式、语言、确认键和 FPS 显示，然后启动。发布包无需 `run.bat`。

包根目录 `launcher.json` 提供游戏路径和首次菜单默认值：

```json
{
  "game_path": "game",
  "mode": "pro4k",
  "display": "borderless"
}
```

`game_path` 支持游戏目录或 `eboot.bin`：例如 `game/CUSA00309`、`D:/Games/inFAMOUS/CUSA00309`、`D:/Games/inFAMOUS/CUSA00309/eboot.bin`。相对路径以包根目录为准；存在多个副本时必须明确指定。也可用命令行临时覆盖：

```powershell
& ".\inFAMOUS Second Son.exe" --game "D:/Games/inFAMOUS/CUSA00309" --mode 1080p
```

菜单提供 720p、900p、1080p、1440p、1800p、Pro 4K；当前按 16:9 适配。全屏／无边框窗口覆盖当前显示器，窗口模式默认最大化。`display` 对应 `fullscreen`、`borderless`、`window`。

**游戏版本兼容性：** Release 接受 Second Son 的 CUSA00004、CUSA00223、CUSA00305、CUSA00309，不接受 First Light 或其他游戏。按最终挂载的游戏（含更新层）的标题、APP_VER 和完整程序 SHA256 匹配 `profiles/second-son-builds.json`。已核验港版 01.07 可使用完整分辨率补丁；其他地区只有程序指纹完全一致才复用补丁，仍待地区实测。不同指纹／更新版本回退到基础 PS4 1080p、精确回读与 SDR，关闭 Pro 和固定地址 XML 补丁，不承诺其画面、性能及存档兼容性。main／开发分支目前仍针对 CUSA00309 01.07。

### 玩家运行依赖

| 依赖 | lite 精简包 | portable 共享包 |
| --- | --- | --- |
| Windows x64、支持 x86-64-v3（含 AVX2）的 CPU | 需要 | 需要 |
| GPU 官方驱动，支持 Vulkan 1.3+ 及引擎要求的功能／扩展 | 需要；无需 Vulkan SDK | 相同 |
| 系统 .NET Framework 4.8，供 C# 启动菜单使用 | 需要；无需 .NET SDK | 相同 |
| Microsoft Visual C++ v14 **x64** 运行库 | 由系统安装提供 | 包内携带所需的四个 DLL |
| Python、Java、编译器、模拟器 GUI、性能采集工具 | **均不需要** | **均不需要** |

已核对的 2026-10-19 V2 Release 构建（EXE 文件版本 0.18.1.0）使用 VC++ DLL **14.51.36247.0**；lite 建议安装该版本或更新的 [微软官方 x64 运行库](https://aka.ms/vc14/vc_redist.x64.exe)。这不是对所有旧版运行库的最低版本验证，后续包应核对 `release-manifest.json` 中的依赖版本与 `runtime_sha256`，不能仅凭 EXE 文件版本判断。

portable 附带 `MSVCP140.dll`、`MSVCP140_ATOMIC_WAIT.dll`、`VCRUNTIME140.dll`、`VCRUNTIME140_1.dll`，应保持在主 EXE 旁。FFmpeg、SDL、OpenAL、FreeType 等库已静态链接到当前引擎，因此两种包都无需安装 FFmpeg 或 Python。Windows 系统组件和显卡驱动由运行电脑提供；项目没有自动依赖安装向导。

## 从 Release 分支构建与打包

先切换 `release/optimization-v2`，再执行对应入口：

| 版本 | 构建 | 打包 | 输出 |
| --- | --- | --- | --- |
| 精简版 | `build-lite.bat` | `release-lite.bat` | `out/release/lite-时间-编号/` 及同名 ZIP |
| 共享版 | `build-portable.bat` | `release-portable.bat` | `out/release/portable-时间-编号/` 及同名 ZIP |

两套入口使用同一 Release 内核与 `Build/Release/`，产物为主 EXE 和 `support/Ifss.Launcher.exe`；差别在运行库打包策略，可以复用同一次最新构建分别打包。先构建再打包，打包不自动构建或启动游戏。脚本校验源码／产物指纹与依赖，保留完整 `Build/Release/build.log`，交互控制台单行刷新编译进度，警告和错误完整输出。

发布包包含干净配置、兼容性配置／补丁、许可证和逐文件校验清单；不包含游戏、个人存档、源码、PDB、SDK 或诊断脚本。两版都无需目标玩家安装 Python。更新时保留自己的 `state/` 和游戏路径配置，成套更新主 EXE、`support/` 与 `profiles/`，不要用干净配置覆盖个人数据。

## 开发与诊断

main／开发分支先执行 `build.bat`，再执行 `run.bat`。默认编译为带优化和调试符号的 `Build/RelWithDebInfo/inFAMOUS Second Son.exe` 与 PDB。`build.bat --check` 只检查环境。**C++ 源码修改后必须重新构建，启动脚本不实时编译，也不调用其他项目的旧 EXE。**

构建复用系统现有 CMake 3.30+、Ninja、Visual Studio LLVM/Clang-cl（C++23）、Windows SDK 和 Windows .NET 编译工具；第三方源码及 FFmpeg 库位置由 `project.json` 的 `dependencies_root`、`dependency_build_root` 指定，目前保留外部共享依赖。克隆仓库不等于依赖齐备，需配置已有环境；脚本不自动安装依赖。上游构建生成步骤及开发分析／测试仍可能使用 Python，这与发布包玩家无需 Python 是两回事。首次引擎构建的中间文件通常占 1–3 GiB。

| 开发入口 | 用途 |
| --- | --- |
| `run.bat` | 启动菜单与普通游玩；`run.bat pro4k` 可预选 4K |
| `run.bat --debug` | 等待已有原生调试器附加，使用同次构建的 PDB |
| `check.bat`／`test.bat` | 配置／补丁预检和离线回归，不启动游戏 |
| `diagnostics/optimization-v3-control.bat`／`optimization-v3.bat` | V2 基线与当前 C1 候选的人工对照，均保留 V1/V2 |
| `diagnostics/full.bat`／`full-optimization-v3.bat` | 同一游戏进程、同次运行的自动采集；后者开启当前 C1 候选 |

专项调试复用已安装的 WPR/WPA、VTune、Nsight Graphics 或 RenderDoc 等工具，部分采集需要管理员权限。full 按设置选择 Nsight 或 RenderDoc，不能宣称互斥工具全部同时开启；结果及缺失项保存在本轮 `diagnostics/coverage.json`、`data-quality.json` 等记录中，必须先检查完整性再分析。

诊断结果位于 `state/sessions/`，无单轮容量上限，监控空间并在剩余低于 2 GiB 时请求停止。每次诊断启动前，采集区超过 20 GB 时清理旧 session，仅保留最近一轮；普通游玩不触发该清理。重要采集请自行留存。性能收益应以同一存档、视角、输入、画质和预热条件下的普通运行对照为依据。

## 存档、设置与按键

以下路径均相对于项目／发布包目录，不从 AppData 自动导入：

| 路径 | 内容 |
| --- | --- |
| `state/play/home/` | 固定正式存档根；游戏存档位于 `1000/savedata/<地区编号>/` |
| `state/play/user/config.json` | 引擎配置及同目录用户文件 |
| `state/play/launcher.json` | 菜单选择的分辨率、显示、语言、确认键和 FPS 偏好 |
| `state/runtime/` | 可复用缓存；Release 对不同游戏构建进行缓存隔离 |
| `state/sessions/` | 开发运行／诊断会话，Release 正常运行不依赖它 |

开发分支普通游玩使用固定存档的会话副本，正常结束并通过文件检查后回存；诊断和 V3 实验不会自动覆盖正式存档。Release 直接使用包内固定存档根。地区之间不自动转换存档。备份或搬迁时保留整个 `state/`；`game/`、`state/`、`Build/`、`out/`、`doc/` 均忽略 Git。

F1 打开退出确认，Enter／手柄 ×（Xbox A）确认，Esc／○（Xbox B）返回；Alt+F4 退出。F10 切换 FPS，F12 截图。FPS 是最近最多 60 个新游戏帧的平均值，帧时间为平均帧间隔，不是纯 GPU 时间；Release 保留此基础显示，不启动采集器。退出前请在游戏内保存。

## 来源与许可

保留 shadPS4 Emulator Project、AYOUB1080p 及各源文件的版权声明，采用 GPL-2.0-or-later，见 [LICENSE](LICENSE) 与 [LICENSES](LICENSES/)。分发修改后的二进制时应遵守对应源码提供义务；第三方许可随包位于 `third-party-licenses/`。项目不分发游戏或个人存档。
