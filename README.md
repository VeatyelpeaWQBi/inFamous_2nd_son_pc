# inFAMOUS Second Son PC

[简体中文](#简体中文) | [English](#english)

## 简体中文

基于 shadPS4 / AYOUB1080p GPL 源码的 Windows x86-64 游戏专用引擎，面向《inFAMOUS Second Son》的 1080p 与 Pro 4K 游玩和性能优化。它在 Windows 上运行 PS4 游戏程序，并非取得游戏源码后重写的 PC 移植版，也不是通用 PS4 启动器。

游戏文件由使用者自行提供，项目和发布包均不附带游戏。原始文件保持只读，游戏补丁只作用于运行中的程序，并检查版本、程序指纹和原始字节。

### 分支与优化状态

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

V3 仍为实验：R6 光照深度归约已确认命中，但未证明稳定整帧收益；R7 光照纯表达式复用、R8 完整窗口预取和 R9 排序数组写上界均完成独立普通 A/B/A，未验收新收益，未纳入默认内核；R9 候选表现偏低。R10 完成提交间隔复核及 64 个普通光照 GPU 样本，确认约 4.34ms 的计时括号。R11 已实现受对齐和实际描述符边界保护的光照四字向量读取，确认 31 组命中并通过实际 SPIR-V 验证；普通 B/A/B 为 20.31/26.88/28.25 FPS，波动较大，未验收稳定收益，默认关闭。后续优先补足稳定对照条件和完整反馈消费依赖证据。A1 等候选也不能当作已验收成果。性能测试入口自动静音，普通游玩音量保留。普通 `run.bat` 和 `diagnostics/full.bat` 默认关闭 V3，Release 不包含这些候选。

R12 已补入低频温度、时钟及进程 CPU 观测，并完成静音 A/B/B/A。发现浏览器与游戏存在同 GPU 引擎并发负载；两次候选仍不稳定，未新增已验收收益。原始程序的 26 处消费指令核验通过，完整反馈依赖及 CPU 副作用继续保留。

R13 已加入默认关闭的有界消费现场记录。64个计数读取样本中，32个对象寄存器地址对应53个反馈指针值；地址相等不足以证明逻辑对象或数据代数相同。辅助记录读取仍有观测盲区，完整回读与CPU副作用继续保留，未新增已验收FPS收益。

R14 已核验辅助提交链的19个有界窗口、62处原始指令及两个导入函数：两次记录操作构造GPU DMA，完成序号和命令空间回收使用不同字段，共享记录池还存在直接CPU写入。新增 `scripts/analyze_auxiliary.py` 可复算这些证据；实际任务代数仍未证明。本轮未改引擎、未新增性能测试或已验收收益，当前原生构建身份及零编译诊断状态已复核。

R15 新增默认关闭的2/4路完整提交录制、同命令copy region合并和精确回读完成交接，并补齐19项独立选择、两组组合及双分辨率测试矩阵。构建warning/error为0/0，12个原生回归目标、63项离线分析及29个准备入口核验通过，尚未启动游戏验收。`diagnostics/v3-matrix.bat` 默认只生成计划；性能测试自动静音。GPU内消费、同代成组快照、客体内存直写等仍缺生命周期证明，未宣称完成或获益。详见本地 `doc/50-r15-remaining-candidates-and-test-matrix.md`。

新增用户自行启动的 `test-v3-all.bat`：先构建/回归，再自动识别进入游戏、静音预热，完成双分辨率单项/组合、数值候选重复验证及 full/机制记录，最后写本地完整日志。运行不需要 Codex 或 AI 调用。结果入口为 `state/runtime/v3-suite-last.json`，停止入口为 `stop-v3-tests.bat`；配置见 `diagnostics/v3-suite.json`。本轮仅实现与静态检查，新代码尚未构建或运行测试；结构性研究仍未全部完成。详见本地 `doc/52-unattended-v3-suite-implementation.md`。

### 测试平台与运行表现

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

这些记录说明用户已感知 V1/V2 的收益，但不是严格同场景 A/B 测试，不能换算成统一提升百分比，也不代表全游戏最低帧率。Release 包的逐场景表现仍需另行验证。full 的验证层、采样和抓帧会显著拖慢运行，其 FPS 不能代表正常游玩，也不能简单扣除固定开销还原真实帧时间。

### 预热与缓存

- 首次启动、进入尚未访问的场景，或相关缓存因变更而失效后，可能触发着色器／管线编译并产生短暂卡顿。
- 先正常游玩让缓存逐步建立，待场景与帧率趋稳后再评估性能。没有统一的预热时长；已预热场景也不保证之后没有新编译。
- 可复用缓存位于 `state/runtime/`。更新或搬迁时保留 `state/`；常规清空缓存会导致重新建立。Release 按游戏构建隔离缓存，不要手工混用不同游戏程序的缓存。

### 使用 Release 包

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

#### 玩家运行依赖

| 依赖 | lite 精简包 | portable 共享包 |
| --- | --- | --- |
| Windows x64、支持 x86-64-v3（含 AVX2）的 CPU | 需要 | 需要 |
| GPU 官方驱动，支持 Vulkan 1.3+ 及引擎要求的功能／扩展 | 需要；无需 Vulkan SDK | 相同 |
| 系统 .NET Framework 4.8，供 C# 启动菜单使用 | 需要；无需 .NET SDK | 相同 |
| Microsoft Visual C++ v14 **x64** 运行库 | 由系统安装提供 | 包内携带所需的四个 DLL |
| Python、Java、编译器、模拟器 GUI、性能采集工具 | **均不需要** | **均不需要** |

已核对的 2026-10-19 V2 Release 构建（EXE 文件版本 0.18.1.0）使用 VC++ DLL **14.51.36247.0**；lite 建议安装该版本或更新的 [微软官方 x64 运行库](https://aka.ms/vc14/vc_redist.x64.exe)。这不是对所有旧版运行库的最低版本验证，后续包应核对 `release-manifest.json` 中的依赖版本与 `runtime_sha256`，不能仅凭 EXE 文件版本判断。

portable 附带 `MSVCP140.dll`、`MSVCP140_ATOMIC_WAIT.dll`、`VCRUNTIME140.dll`、`VCRUNTIME140_1.dll`，应保持在主 EXE 旁。FFmpeg、SDL、OpenAL、FreeType 等库已静态链接到当前引擎，因此两种包都无需安装 FFmpeg 或 Python。Windows 系统组件和显卡驱动由运行电脑提供；项目没有自动依赖安装向导。

### 从 Release 分支构建与打包

先切换 `release/optimization-v2`，再执行对应入口：

| 版本 | 构建 | 打包 | 输出 |
| --- | --- | --- | --- |
| 精简版 | `build-lite.bat` | `release-lite.bat` | `out/release/lite-时间-编号/` 及同名 ZIP |
| 共享版 | `build-portable.bat` | `release-portable.bat` | `out/release/portable-时间-编号/` 及同名 ZIP |

两套入口使用同一 Release 内核与 `Build/Release/`，产物为主 EXE 和 `support/Ifss.Launcher.exe`；差别在运行库打包策略，可以复用同一次最新构建分别打包。先构建再打包，打包不自动构建或启动游戏。脚本校验源码／产物指纹与依赖，保留完整 `Build/Release/build.log`，交互控制台单行刷新编译进度，警告和错误完整输出。

发布包包含干净配置、兼容性配置／补丁、许可证和逐文件校验清单；不包含游戏、个人存档、源码、PDB、SDK 或诊断脚本。两版都无需目标玩家安装 Python。更新时保留自己的 `state/` 和游戏路径配置，成套更新主 EXE、`support/` 与 `profiles/`，不要用干净配置覆盖个人数据。

### 开发与诊断

main／开发分支先执行 `build.bat`，再执行 `run.bat`。默认编译为带优化和调试符号的 `Build/RelWithDebInfo/inFAMOUS Second Son.exe` 与 PDB。`build.bat --check` 只检查环境。**C++ 源码修改后必须重新构建，启动脚本不实时编译，也不调用其他项目的旧 EXE。**

构建复用系统现有 CMake 3.30+、Ninja、Visual Studio LLVM/Clang-cl（C++23）、Windows SDK 和 Windows .NET 编译工具。**项目所需依赖已归档在本项目内，不再读取旧工程目录**：源码／头文件在 `externals/`，六个 FFmpeg 5.1.2 静态库在 `externals/prebuilt/ffmpeg/lib/`。`dependencies.lock.json` 固定 54 个源码组件（含嵌套依赖）、10 个基础文件及库的版本／哈希；`externals/dependency-inventory.tsv` 记录 41,489 个文件的校验信息。源码和库约 879 MiB，含保留的下载归档及元数据共约 1.15 GiB，未使用指向项目外的链接。构建逐文件核对本地归档，缺失或变更即停止，不回退兄弟工程或自动下载。本次迁移已完成文件及配置审计，尚未执行构建／游戏验收。

完整复制或备份项目时须包含被 Git 忽略的 `externals/`。仅克隆 Git 时，可在明确允许下载后用系统 PowerShell 执行 `scripts/restore_dependencies.ps1`；它只使用本项目的锁定清单及 `third_party/bootstrap/`，不需要旧工程。日常构建使用 `build.bat`，不会调用恢复脚本。来源及许可见 `third_party/README.md`。上游构建生成步骤及开发分析／测试仍可能使用 Python，这与发布包玩家无需 Python 是两回事。每份引擎构建的中间文件通常占 1–3 GiB。

| 开发入口 | 用途 |
| --- | --- |
| `run.bat` | 启动菜单与普通游玩；`run.bat pro4k` 可预选 4K |
| `run.bat --debug` | 等待已有原生调试器附加，使用同次构建的 PDB |
| `check.bat`／`test.bat` | 配置／补丁预检和离线回归，不启动游戏 |
| `diagnostics/optimization-v3-control.bat`／`optimization-v3.bat` | V2 基线与当前 C1 候选的人工对照，均保留 V1/V2 |
| `diagnostics/full.bat`／`full-optimization-v3.bat` | 同一游戏进程、同次运行的自动采集；后者开启当前 C1 候选 |

专项调试复用已安装的 WPR/WPA、VTune、Nsight Graphics 或 RenderDoc 等工具，部分采集需要管理员权限。full 按设置选择 Nsight 或 RenderDoc，不能宣称互斥工具全部同时开启；结果及缺失项保存在本轮 `diagnostics/coverage.json`、`data-quality.json` 等记录中，必须先检查完整性再分析。

诊断结果位于 `state/sessions/`，无单轮容量上限，监控空间并在剩余低于 2 GiB 时请求停止。每次诊断启动前，采集区超过 20 GB 时清理旧 session，仅保留最近一轮；普通游玩不触发该清理。重要采集请自行留存。性能收益应以同一存档、视角、输入、画质和预热条件下的普通运行对照为依据。

### 存档、设置与按键

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

### Release 手柄改键测试版

Release 使用 `launcher.json`；开发分支和 main 使用 `project.json`。其中，`"xbox_prompts": true` 和 `"touchpad_chords": true` 表示允许对识别出的 Xbox 手柄启用相应改动，可分别设为 `false` 后重启禁用；不会强制对 PS 手柄启用。lite 与 portable 使用相同逻辑，不增加玩家运行依赖。

- **自动识别：** 以游戏启动时实际分配给玩家 1 的手柄为准。PS3／PS4／PS5 保持原提示与操作；Xbox 360／Xbox One 类设备按配置启用两项改动；未连接、未知及其他类型默认不启用。请先连接手柄再启动，启动后才连接或换系列需重启游戏，使已缓存的提示和补丁选择一致。Xbox 会话中拔出或换成 PS 手柄会立即旁路组合键，并清除旧的组合状态。DS4Windows／Steam Input 等若将 PS 手柄呈现为虚拟 Xbox，按这个对外类型识别，无法保证识别出背后的实体手柄。

- **共用 Xbox 图标：** 在已核验中／英／韩语言包加载时，统一替换共用图集里的 13 个按钮字形（A/B/X/Y、LB/LT/RB/RT、MENU、LS/RS 与按下状态）。不再改写零散的文字标签和程序字形表；保留字形编号、尺寸、间距与游戏绑定解析，确认／取消继续跟随菜单设置。触摸板按压说明按通用词组更新：单指显示两组组合键二选一，双拇指显示四键同时按；三种语言各发现 14 条对应文案。滑动说明保留原操作；未确认的 F089 字形不修改。
- **触摸板指纹图案：** 已核验的共用交互资源在内存中改为宽排三列：左列 `LT / + / LB`，右列 `RT / + / RB`，单指任一侧可用时中央为 **OR**，双指同时按时为 **AND**。去掉额外边框，按触摸板底图范围扩大并调整提示网格及其显示边界，保留背景、索引、颜色、动画和资源尺寸；同步重建全部 9 级 mip。绘制器另支持仅左／仅右，只有确认资源要求后才接入；当前已定位的两个图形网格不含左右限定标识，不根据玩家已按哪侧猜测。资源加载时使用完整 SHA256 校验的内存快照，core_common 句柄打开期间额外占用约 303 MiB，句柄关闭后释放；不新增逐帧扫描或磁盘缓存。其他独立手掌／滑动教学动画尚未全部替换，不宣称全场景覆盖。
- **LT+LB / RT+RB：** 分别对应游戏内部的左侧／右侧触摸板按压，两组同按对应双指按压。扳机需按到一半以上；按住组合可维持按压。滑动、鼠标体感和原触摸板操作不变。
- 在游戏自身输入转换、触点数和坐标读取处应用内存钩子，不修改 SDL 或 `libScePad` 输入报告。组合成立时屏蔽同组原动作和模拟量，释放阶段持续屏蔽，直到同组两键均松开。
- 同组首个键有约 **80 毫秒识别窗口**；短单击延后补发，单独长按在窗口后恢复。两键间隔超过窗口时，第一个键的动作可能已经执行，无法撤销。低帧率还会增加按游戏更新周期采样的等待。
- 原始游戏和资源保持只读，只接受 `01.07` 且程序 SHA256 为 `2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab` 的已声明 Second Son 地区。资源另验完整 SHA256；不同资源保留原提示。未识别的输入模式 2 不启用组合键。
- 组合键、普通 Xbox 图标及当前单指提示布局已有用户实测认可；双指提示和其他剧情场景仍需逐项验证。构建后可选离线回归：`cmake --build Build/Release --target ifss_second_son_controls_test`，随后运行 `Build/Release/ifss_second_son_controls_test.exe`。建议测试：单指吸收、双指推车、提示尺寸及动画、两种释放顺序，以及原 RT/RB/LT/LB 的短按和长按。它仍通过游戏现有的触摸交互判定执行动作，没有把所有手势改为普通按键动作。

### 来源与许可

保留 shadPS4 Emulator Project、AYOUB1080p 及各源文件的版权声明，采用 GPL-2.0-or-later，见 [LICENSE](LICENSE) 与 [LICENSES](LICENSES/)。分发修改后的二进制时应遵守对应源码提供义务；第三方许可随包位于 `third-party-licenses/`。项目不分发游戏或个人存档。

## English

A Windows x86-64 engine dedicated to 1080p and Pro 4K gameplay and optimization for **inFAMOUS Second Son**, based on the GPL source of shadPS4 / AYOUB1080p. It runs the PS4 game program on Windows; it is not a PC port rewritten from the game's source code or a general-purpose PS4 launcher.

You must supply the game files. Neither the repository nor the release packages include them. Original files remain read-only; game patches modify the running program only, with version, executable fingerprint and original-byte checks.

### Branches and optimization status

| Branch | Purpose | Launch |
| --- | --- | --- |
| `main` | Source baseline synchronized with development, including V1/V2 and diagnostics; not unmodified upstream | Build, then run `run.bat` |
| `feature/fps-optimization` | Performance research, V3 candidates and controlled comparisons | `run.bat` and specialized entries under `diagnostics/` |
| `release/optimization-v2` | Retains accepted V1/V2; removes V3, sampling, debug panels and external collector integration for regular play | Open `inFAMOUS Second Son.exe` directly to enter the launch menu |

V1/V2 are built into the engine and require no switches:

| Optimization | Purpose and correctness safeguards |
| --- | --- |
| V1: vertex/index buffer binding deduplication | Skips identical bindings within the same command buffer; retains uploads, barriers and state invalidation |
| V1: precise readback range dependencies | Waits for GPU producers relevant to the actual copy range; falls back conservatively for unknown writes or version changes |
| V2: required compute pipeline compilation priority | Prioritizes pipelines the game currently needs over background compilation; retains bounded queues, fair scheduling and every dispatch |
| V2: prefetch based on actual CPU page reads | Selects pages from observed CPU reads to reduce unnecessary readback; retains size, age, version and completion checks |

Precise readback, SDR, complete particle effects, and shader/pipeline caches are retained. Verified game builds also use resolution, light-list capacity, 4K exposure-buffer capacity and anti-freeze patches. Relaxed readback can affect particles and brightness; its FPS does not represent performance with complete rendering.

V3 remains experimental. R6 lighting depth reduction was confirmed active without established whole-frame gains. R7 exact lighting-expression reuse, R8 full-window prefetch, and R9 sorting-array write bounds each completed an independent ordinary A/B/A comparison without accepted new gains; all remain excluded from the default core, with R9 performing below its controls. R10 reviewed submission spacing and collected 64 ordinary lighting GPU samples, confirming an approximately 4.34ms timing bracket. R11 implemented four-word lighting vector reads guarded by alignment and actual descriptor bounds, confirmed 31 packed groups, and validated the generated SPIR-V. Its ordinary B/A/B measured 20.31/26.88/28.25 FPS with substantial variation; no stable gain is accepted and the candidate remains disabled by default. Further research prioritizes stable comparison conditions and complete feedback-consumer dependency evidence. A1 and other candidates are not accepted performance results either. Performance test sessions are automatically muted; ordinary play keeps its volume. Ordinary `run.bat` and `diagnostics/full.bat` disable V3 by default. Release excludes these candidates.

R12 added low-rate temperature, clock and process CPU observations and completed a muted A/B/B/A. A spot check found browser activity on the same GPU engine as the game. Candidate repetitions remained inconsistent, with no new accepted gain. Twenty-six consumer instruction witnesses matched the original program; complete feedback dependencies and CPU side effects remain preserved.

R13 added bounded consumer fault-context evidence, disabled by default. In 64 count-read samples, 32 object-register addresses were associated with 53 feedback pointer values. Address equality does not establish logical identity or data generation. Auxiliary record consumption remains incompletely observed; complete readback and CPU side effects are preserved, with no new accepted FPS gain.

R14 verified 19 bounded auxiliary-submission windows, 62 original instruction witnesses and two imports. The record operations construct GPU DMA commands; completion sequences and command-space reclamation use separate fields, and the shared record pool also has direct CPU writers. `scripts/analyze_auxiliary.py` reproduces this evidence. Actual task generations remain unproven. The engine was unchanged, no new performance run or gain was claimed, and the current native build identity and zero compiler diagnostics were rechecked.

R15 adds disabled-by-default 2/4-worker complete-session recording, within-command copy-region coalescing and exact readback-completion handoff, with 19 independent selections, two combinations and a matrix for both resolutions. The build has zero compiler warnings/errors; 12 native regression targets, 63 offline analysis tests and 29 prepared entries passed. No game acceptance run has started. `diagnostics/v3-matrix.bat` only creates a plan by default; performance tests are muted. GPU-only consumption, same-generation grouped snapshots and direct guest-memory writes still lack lifecycle proof and are not claimed implemented or beneficial. See local `doc/50-r15-remaining-candidates-and-test-matrix.md`.

The user-started `test-v3-all.bat` builds/checks the project, automatically verifies scene readiness, runs muted comparisons and confirmation groups at both resolutions, collects full/mechanism evidence and saves complete local reports. It uses no Codex or AI calls while running. `state/runtime/v3-suite-last.json` points to the latest reports; use `stop-v3-tests.bat` to request a stop and `diagnostics/v3-suite.json` for configuration. This implementation has only received static source checks; it has not been built or tested. Structural research remains incomplete. See local `doc/52-unattended-v3-suite-implementation.md`.

### Test platform and observed performance

Hardware and system details checked on 2026-10-19:

| Component | Configuration |
| --- | --- |
| Computer | ASUS ROG Strix SCAR 18 G834JYR |
| CPU | Intel Core i9-14900HX, 24 cores / 32 threads |
| GPU | NVIDIA GeForce RTX 4090 **Laptop GPU, 16 GB VRAM**; Intel UHD integrated graphics also present |
| Memory | 64 GB, approximately 63.6 GiB usable by the system |
| OS / GPU driver | Windows 11 Home, Chinese edition, Build 26100; NVIDIA 616.56 |

The following are **manual gameplay observations** from V1/V2 development builds on this machine, using the Hong Kong CUSA00309 01.07 build, precise readback, SDR and complete particles. 4K means Pro 3840×2160 with checkerboard rendering disabled. Scenes, actions and warm-up conditions differ between observations:

| Scene | Observed performance |
| --- | --- |
| Typical 1080p gameplay | Around 60 FPS |
| Typical Pro 4K scenes at the V1 stage | Mostly above 40 FPS, occasionally around 38–40; previously around 30–40 FPS |
| Typical Pro 4K scenes after V2 warm-up | Around 50 FPS or higher |
| V2 movement / ability use; simultaneous abilities by both characters | Around 45+ FPS and 41+ FPS, respectively |
| Opening car-crash scene with heavy fire and smoke | Around 30–35 FPS; further improvement is needed |

These observations reflect user-perceived V1/V2 improvements, not strict same-scene A/B measurements. They do not establish a universal percentage gain or a minimum FPS across the whole game. Release packages still need scene-by-scene testing. Validation layers, sampling and frame capture can substantially slow full diagnostics; their FPS is not normal gameplay performance, and subtracting a fixed overhead cannot recover normal frame times.

### Warm-up and caches

- First launch, previously unseen scenes, or changes that invalidate relevant caches may trigger shader/pipeline compilation and temporary stutter.
- Play normally to let caches build, and assess performance after the scene and frame rate settle. There is no universal warm-up duration; a warmed scene does not rule out later compilation.
- Reusable caches are stored under `state/runtime/`. Preserve `state/` when updating or moving the package; routine cache deletion forces rebuilding. Release isolates caches by game build; do not manually mix caches from different game executables.

### Using a Release package

1. Fully extract `lite-*.zip` or `portable-*.zip`, keeping `support/`, `profiles/` and configuration files.
2. Place the complete game under `game/`, including `eboot.bin` and resources. A regional subdirectory such as `game/CUSA00309/` or an external game path is also supported.
3. Double-click `inFAMOUS Second Son.exe`, select resolution, display mode, language, confirm button and FPS display in the menu, then start. Release packages do not require `run.bat`.

The package-root `launcher.json` supplies the game path and initial menu defaults:

```json
{
  "game_path": "game",
  "mode": "pro4k",
  "display": "borderless"
}
```

`game_path` accepts a game directory or `eboot.bin`, for example `game/CUSA00309`, `D:/Games/inFAMOUS/CUSA00309` or `D:/Games/inFAMOUS/CUSA00309/eboot.bin`. Relative paths are resolved from the package root. Specify the desired copy explicitly when multiple copies exist. A command-line override is also available:

```powershell
& ".\inFAMOUS Second Son.exe" --game "D:/Games/inFAMOUS/CUSA00309" --mode 1080p
```

The menu offers 720p, 900p, 1080p, 1440p, 1800p and Pro 4K, currently targeting 16:9. Fullscreen and borderless modes cover the current monitor; windowed mode starts maximized. The `display` values are `fullscreen`, `borderless` and `window`.

**Game compatibility:** Release accepts Second Son IDs CUSA00004, CUSA00223, CUSA00305 and CUSA00309; it rejects First Light and other games. The final mounted game, including updates, is matched by title, APP_VER and full executable SHA256 against `profiles/second-son-builds.json`. The verified Hong Kong 01.07 executable supports the full resolution patch set. Other regions reuse patches only with an identical executable fingerprint and still require regional testing. Different fingerprints or updates fall back to base PS4 1080p, precise readback and SDR, with Pro and fixed-address XML patches disabled; rendering, performance and save compatibility are not guaranteed for those builds. main/development currently target CUSA00309 01.07.

#### Player requirements

| Dependency | lite | portable |
| --- | --- | --- |
| Windows x64 and an x86-64-v3 CPU, including AVX2 | Required | Required |
| Official GPU driver supporting Vulkan 1.3+ and the engine's required features/extensions | Required; no Vulkan SDK needed | Same |
| System .NET Framework 4.8 for the C# launch menu | Required; no .NET SDK needed | Same |
| Microsoft Visual C++ v14 **x64** runtime | Installed on the system | Four required DLLs included |
| Python, Java, compiler, emulator GUI or profiling tools | **Not required** | **Not required** |

The inspected 2026-10-19 V2 Release builds (EXE file version 0.18.1.0) use VC++ DLL version **14.51.36247.0**. For lite, install that version or newer from [Microsoft's official x64 redistributable](https://aka.ms/vc14/vc_redist.x64.exe). This is not a tested minimum across every older runtime. For later packages, check dependency versions and `runtime_sha256` in `release-manifest.json`, rather than relying on the EXE file version alone.

portable includes `MSVCP140.dll`, `MSVCP140_ATOMIC_WAIT.dll`, `VCRUNTIME140.dll` and `VCRUNTIME140_1.dll`; keep them beside the main EXE. FFmpeg, SDL, OpenAL, FreeType and other libraries are statically linked into the current engine, so neither edition needs a separate FFmpeg or Python installation. Windows components and GPU drivers must be provided by the target system. There is no automatic dependency installation wizard.

### Building and packaging the Release branch

Switch to `release/optimization-v2`, then use the matching entries:

| Edition | Build | Package | Output |
| --- | --- | --- | --- |
| lite | `build-lite.bat` | `release-lite.bat` | `out/release/lite-<timestamp>-<id>/` and a ZIP with the same name |
| portable | `build-portable.bat` | `release-portable.bat` | `out/release/portable-<timestamp>-<id>/` and a ZIP with the same name |

Both entries use the same Release engine and `Build/Release/`, producing the main EXE and `support/Ifss.Launcher.exe`. The difference is runtime packaging; a single current build can produce both editions. Build first, then package. Packaging does not build or launch the game. Scripts verify source/output fingerprints and dependencies, retain the full `Build/Release/build.log`, and refresh compilation progress on one console line while printing all warnings and errors.

Packages contain clean configuration, compatibility profiles/patches, licenses and per-file checksums. They exclude the game, personal saves, source code, PDBs, SDKs and diagnostic scripts. Neither edition requires players to install Python. When updating, preserve your `state/` and game-path configuration, update the main EXE, `support/` and `profiles/` together, and do not overwrite personal data with clean defaults.

### Development and diagnostics

On main/development, run `build.bat`, then `run.bat`. The default optimized build with debug symbols produces `Build/RelWithDebInfo/inFAMOUS Second Son.exe` and its PDB. `build.bat --check` only checks the environment. **C++ source changes require rebuilding; launch scripts do not compile live or invoke an old EXE from another project.**

Builds reuse installed CMake 3.30+, Ninja, Visual Studio LLVM/Clang-cl (C++23), the Windows SDK and Windows .NET compilation tools. **All project dependency inputs are archived here; the old project directory is no longer read.** Sources/headers live under `externals/` and six FFmpeg 5.1.2 static libraries under `externals/prebuilt/ffmpeg/lib/`. `dependencies.lock.json` pins 54 source components including nested dependencies, ten bootstrap files and the binary hashes. `externals/dependency-inventory.tsv` inventories 41,489 files. Sources/libraries occupy about 879 MiB, or 1.15 GiB including retained download archives and metadata, with no filesystem links outside the project. Build preflight checks local files and stops on missing/changed data without external fallback or automatic downloads. File/configuration migration has been audited; a real build/game acceptance run has not been performed.

Whole-project copies and backups must include the ignored `externals/` folder. A Git-only clone can explicitly restore dependencies with the system PowerShell and `scripts/restore_dependencies.ps1` after download consent. Restoration uses this project's lock and `third_party/bootstrap/`, requiring no old checkout. The ordinary `build.bat` never invokes restoration. See `third_party/README.md` for provenance and licensing. Upstream generation steps and developer analysis/tests may still use Python; this is separate from players needing no Python for release packages. Each engine output's build intermediates typically occupy 1–3 GiB.

| Development entry | Purpose |
| --- | --- |
| `run.bat` | Launch menu and regular play; `run.bat pro4k` preselects 4K |
| `run.bat --debug` | Waits for an existing native debugger to attach, using the matching PDB |
| `check.bat` / `test.bat` | Configuration/patch preflight and offline regression checks; no game launch |
| `diagnostics/optimization-v3-control.bat` / `optimization-v3.bat` | Manual comparison of the V2 baseline and current C1 candidate; both retain V1/V2 |
| `diagnostics/full.bat` / `full-optimization-v3.bat` | Automated collection in one game process during one run; the latter enables the current C1 candidate |

Specialized diagnostics reuse installed WPR/WPA, VTune, Nsight Graphics or RenderDoc; some collection requires administrator privileges. full selects Nsight or RenderDoc according to settings, not both mutually exclusive tools simultaneously. Results and missing coverage are recorded in the session's `diagnostics/coverage.json`, `data-quality.json` and related files. Check completeness before analysis.

Diagnostic output is stored under `state/sessions/`, with no per-run size cap. Free space is monitored and a stop is requested below 2 GiB. Before a diagnostic launch, if the collection area exceeds 20 GB, older sessions are removed while the most recent one is retained; regular play does not trigger this cleanup. Back up important captures. Assess improvements through normal gameplay comparisons with the same save, viewpoint, input, rendering settings and warm-up conditions.

### Saves, settings and controls

Paths below are relative to the project/package directory. No automatic import from AppData is performed:

| Path | Contents |
| --- | --- |
| `state/play/home/` | Fixed persistent save root; game saves are under `1000/savedata/<title-id>/` |
| `state/play/user/config.json` | Engine configuration and user files in the same directory |
| `state/play/launcher.json` | Saved resolution, display mode, language, confirm button and FPS preferences |
| `state/runtime/` | Reusable caches; Release isolates caches for different game builds |
| `state/sessions/` | Development/diagnostic sessions; normal Release play does not depend on it |

Ordinary development play uses a session copy of the fixed save, copied back after a normal exit and file checks. Diagnostics and V3 experiments do not automatically overwrite persistent saves. Release uses the fixed package-local save root directly. Saves are not automatically converted across regions. Preserve the entire `state/` when backing up or moving the project. `game/`, `state/`, `Build/`, `out/` and `doc/` are ignored by Git.

F1 opens exit confirmation; Enter / controller × (Xbox A) confirms, and Esc / ○ (Xbox B) returns. Alt+F4 exits. F10 toggles FPS; F12 takes a screenshot. FPS averages the latest up to 60 new game frames, and frame time is the mean frame interval, not pure GPU time. Release retains this basic display without starting collectors. Save in-game before exiting.

### Release controller remapping test version

Release reads `launcher.json`; development and main read `project.json`. In either file, `"xbox_prompts": true` and `"touchpad_chords": true` allow the respective change for recognized Xbox devices. Set either to `false` and restart to disable it; these switches do not force changes onto PlayStation devices. Both packages share the implementation and add no player dependency.

- **Automatic detection:** use the device actually assigned to player 1 at startup. PS3/PS4/PS5 keep their original prompts and controls; Xbox 360/One families enable the configured changes; absent, unknown and other devices default to disabled. Connect before starting. Late connections or family changes require a restart so cached prompts and patch selection agree. During an Xbox session, disconnecting or replacing the device with a PS controller bypasses chords and discards stale chord state. If DS4Windows/Steam Input exposes a PS controller as a virtual Xbox device, detection follows that exposed type; the hidden physical model cannot be guaranteed.

- **Shared Xbox icons:** while loading validated Chinese, English and Korean packs, replace 13 shared button glyphs (A/B/X/Y, LB/LT/RB/RT, MENU, LS/RS and stick clicks) in RAM. Glyph IDs, dimensions, advances and the game binding resolver remain intact; confirm/cancel follows the menu preference. The previous per-string ASCII substitutions and executable glyph-table edits are removed. Common phrase rules update touchpad-button instructions: either chord for one finger, all four buttons together for two thumbs (14 matching values per language). Swipe instructions retain their actual gesture. Unidentified F089 remains unchanged.
- **Touchpad fingerprints:** validated shared artwork is replaced in RAM with a wide three-column layout: `LT / + / LB` on the left, `RT / + / RB` on the right, and **OR** for either-side presses or **AND** for simultaneous two-finger presses. No extra border. Overlay geometry and its culling bounds expand to fit the touchpad artwork; the background, indices, colors, animation and resource dimensions remain intact. All nine mips are rebuilt. The renderer also supports left-only and right-only requirements, but those require positively identified resources; the two identified meshes have no side-specific identifier, so the player's pressed side is not used to guess the required action. A full SHA-256-validated snapshot uses about 303 MiB while core_common is open and is released when its handle closes; no per-frame scan or disk cache is added. Standalone hand/swipe tutorial artwork is not fully covered.
- **LT+LB / RT+RB:** left/right game-side touchpad presses; both chords together produce a two-finger press. Pull triggers at least halfway. Hold the chords to maintain contact. Existing touchpad, swipe and mouse motion controls remain available.
- Version-guarded hooks change the game's input conversion, contact count and coordinate accessors, without modifying SDL or `libScePad` reports. Original actions and trigger values are suppressed during a chord and its release until both keys on that side are released.
- The first key has an **80 ms arbitration window**. Short single taps are replayed after release; single holds resume after the window. If the second key arrives later, the first action may already have fired and cannot be undone. Slow game updates may add sampling latency.
- Only declared Second Son titles with APP_VER `01.07` and executable SHA256 `2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab` receive these hooks. Resource files are separately hash-checked; different packs retain their original prompts. Unidentified input mode 2 does not enable chords. Original files remain read-only.
- Chords, ordinary Xbox icons and the current single-finger hint layout have received user acceptance. Two-finger hints and other story interactions still require scenario-specific validation. After building, optional offline regression: `cmake --build Build/Release --target ifss_second_son_controls_test`, then run `Build/Release/ifss_second_son_controls_test.exe`. Check single-finger absorption, two-finger pushes, hint sizing/animation, both release orders and original single-button taps/holds. Existing game touch interaction predicates still execute the actions; this does not rebind every gesture to a normal button action.

### Credits and license

Copyright notices for the shadPS4 Emulator Project, AYOUB1080p and individual source files are retained. The project uses GPL-2.0-or-later; see [LICENSE](LICENSE) and [LICENSES](LICENSES/). Distributing modified binaries requires compliance with the corresponding-source obligations. Third-party licenses are included under `third-party-licenses/`. This project does not distribute the game or personal saves.
