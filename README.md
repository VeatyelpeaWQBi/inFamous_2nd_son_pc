# inFamous_2nd_son_pc

Windows x86-64 / inFAMOUS Second Son 港版 CUSA00309 01.07 的适配项目，使用 BAT 作为构建和启动入口。

源码保留 shadPS4 的版权和 GPL 许可；原始游戏文件只读。运行引擎只接受本项目 Build/ 中的构建产物。用户运行 build.bat 才会编译 C++ 源码；缺少构建产物、构建记录或源码指纹不一致时拒绝启动，不调用外部旧 EXE。PowerShell、Python 和补丁配置按脚本执行，不能代替 C++ 编译。

## 构建

运行 build.bat 使用系统现有 CMake、Ninja、Visual Studio Clang 和 Windows SDK，默认以 RelWithDebInfo 编译本项目；包含优化和调试符号。后续复用 Build/ 中的中间文件进行增量构建，不删除缓存，不自动启动游戏。build.bat --check 仍只检查工具，不编译。

默认产物为 Build/RelWithDebInfo/inFAMOUS Second Son.exe 和同名 PDB。首次构建预计新增约 1–3 GiB；构建前要求至少 3 GiB 可用空间。按用户确认，开发阶段继续复用兄弟项目的 externals 源码及 FFmpeg 库，整体 release 打包时再考虑迁入，不自动复制或下载。FFmpeg 库供引擎的 avplayer、videodec 等模块处理视频，不运行外部 ffmpeg.exe。

系统编译器、Python 和诊断工具继续使用现有安装。构建工具按 PATH、用户／系统 PATH、本项目 CMake 缓存及现有安装目录定位，已去除对兄弟项目 CMakeCache.txt 的定位依赖。

只有配置、编译、链接成功，且构建前后本项目源码指纹一致，才会原子更新 project.json，令 run.bat 和诊断入口选用项目内新 EXE，并记录 EXE、源码和符号指纹。失败不会切换选择；已有项目内 EXE 若被失败构建覆盖，指纹检查会拒绝启动。再次修改本项目 C++、CMake 文件、构建脚本或资源后，启动检查要求先重新运行 build.bat。源码指纹范围不包含外部依赖内容，不能作为全部第三方文件的完整性证明。构建记录不代表已经验证游戏画面、性能或稳定性。

Visual Studio／CMake 预设也统一输出到 Build/Debug、Build/Release 或 Build/RelWithDebInfo。IDE 直接构建只生成文件，不更新启动清单；随后运行 build.bat，会复用默认配置的构建目录并完成来源检查与激活。不同配置使用各自目录，不混用缓存。

需要源码断点时，完成构建后运行 run.bat --debug，使用已有调试器附加到该进程并加载同一构建的 PDB。该参数等待调试器，不会代替编译，也不自动启动调试器。

## 运行

普通游玩入口为 run.bat，双击后在菜单选择分辨率、语言、确认按键、帧数显示，以及全屏／窗口／无边框窗口。选择保存在 state/play/launcher.json，下次启动继续使用。旧设置保留，首次增加分辨率时继承最近保存的游戏配置，缺少历史配置时默认 1080p。

| 菜单分辨率 | 游戏模式 | 分辨率补丁 |
| --- | --- | --- |
| 1280×720（720p） | PS4 | 项目适配的 Native 720p |
| 1600×900（900p） | PS4 | 项目适配的 Native 900p |
| 1920×1080（1080p） | PS4 | 原生默认尺寸，关闭分辨率补丁 |
| 2560×1440（1440p） | PS4 Pro | 既有 Pro 1440p，取消棋盘格 |
| 3200×1800（1800p） | PS4 Pro | 既有 Pro 1800p，取消棋盘格 |
| 3840×2160（4K） | PS4 Pro | 既有 Pro 4K，取消棋盘格 |

选择同时设置游戏渲染补丁和窗口尺寸。当前仅提供 16:9；16:10、21:9 等比例需要进一步适配。720p、900p 沿用既有 Native 1440p 的十个修改位置与 OriginalHash，只调整尺寸立即数；新增档位的实际画面和稳定性需由用户验证。

全屏及无边框窗口覆盖当前显示器；窗口模式默认最大化，保留标题栏和任务栏，之后可手动调整。所有档位继续使用精确回读、SDR 及既有亮度修复补丁。菜单也提示体感、触摸板及退出操作。游戏内按 F1 打开引擎退出确认；Enter / × / Xbox A 退出，Esc / ○ / Xbox B 返回。F10 显示帧数，F12 截图。请先完成游戏内保存。

- run.bat --no-menu：直接沿用保存的菜单设置。
- run.bat 1080p / run.bat pro4k：预选分辨率，仍可在菜单修改。
- run.bat pro4k --no-menu：此次强制使用 Pro 4K，保留已保存的菜单偏好。
- run.bat --debug：使用同一菜单，并等待现有 Windows 原生调试器附加。
- run.bat --benchmark 1080p / run.bat --benchmark pro4k：运行冻结基线，跳过菜单。
- quit.bat：中文退出确认窗口，发送正常退出请求。

窗口标题、任务栏图标及启动／退出菜单使用游戏名称和图标。隐藏的窗口辅助进程只修改经路径、PID 和创建时间核对的 SDL 窗口属性，不读写游戏内存。本项目构建产物的 EXE 文件名、文件描述与图标使用游戏名称和游戏图标。

## 已并入内核的 V1／V2 优化

V1、V2 已经用户游玩验证收益，现将以下四项作为引擎固定行为，不再通过配置开关开启：

| 固定策略 | 行为与正确性边界 |
| --- | --- |
| 顶点／索引缓冲绑定去重（V1） | 仅省略同一命令缓冲中完全相同的绑定，保留上传、屏障及状态失效处理 |
| 精确回读范围依赖（V1） | 按实际复制范围选择生产者；未知写入、版本变化等情况继续等待完整窗口 |
| 必需计算管线优先编译（V2） | 必需计算任务优先于后台编译，保留有界队列与公平调度，不跳过 dispatch |
| 实际 CPU 读页预取（V2） | 精确回读的预取使用实际发生过 CPU 读取的页，保留数量、大小、有效期、版本和完成检查 |

先运行 `build.bat` 重新编译，再使用 `run.bat` 游玩；`diagnostics/full.bat`、专项诊断和关键路径计时入口自动共用上述内核行为。C++ 修改必须构建后才生效，旧 EXE 不能通过源码指纹检查。初始配置、存档副本和画面设置仍有各自用途，`--benchmark` 也使用当前构建，不能恢复历史内核。

已移除四个候选开关的解析和旧 V1／V2 组合测试 BAT。历史 `perf_tuning.ini` 中的同名键会被忽略，不能再关闭这些策略；历史会话及实测记录保留。新会话的 `launch.json` 记录 `engine_policy = core-v1-v2`。诊断开关、原有异步回读／预取模式及其他运行设置继续独立生效，精确回读、SDR、粒子和既有补丁的正确性条件保留。普通 FPS 收益仍需在相同场景的普通运行中比较，full 的插桩 FPS 不能代替游玩验收。

集成范围及验证命令见 `doc/23-core-optimization-integration.md`（doc/ 按约定忽略 Git）。

## V3 R6：C1 光照深度归约候选

先执行 `build.bat`。本轮候选只开启 C1：将光照 shader `ce1675e0` 开头逐线程争用同一共享地址的深度 min/max 原子操作，改为原生 GPU 子组规约后，由非空子组的一个线程提交相同结果。保留原屏障、有效像素判断、完整灯光列表、粒子及画质，不改变浮点运算顺序。

| diagnostics 中的入口 | 当前用途 |
| --- | --- |
| optimization-v3-control.bat | 同一构建的 V2 对照，C1、A1 及旧 V3 候选全部关闭 |
| optimization-v3.bat | 仅开启 C1 的普通游玩候选 |
| full-optimization-v3.bat | 既有同进程 full，开启 C1，自动核对编译记录和最终 SPIR-V |

普通 `run.bat` 与 `diagnostics/full.bat` 默认关闭所有候选。A1 已证实减少绑定条目，但没有确认稳定 FPS 收益，本轮也关闭它；R2/R3 地址调度、回读/提交、粒子异步及布局共享继续关闭，V1/V2 固定策略保留。

首先用 control → candidate → control，在同一车祸存档、视角和预热条件下比较 FPS、卡顿、亮度、光照和粒子。默认 Pro 4K；两个人工 BAT 可追加 `-Mode 1080p`。仅目标 shader 使用新的缓存版本；第一次开启或切换后先等待编译预热，不删除共享缓存。普通结果确认后再按需要运行已有 full，不能以 full FPS 代替游玩验收。

候选以完整客体 shader 字节指纹、16×16×1 工作组、硬件 compute 子组能力、共享地址/布局、屏障和无副作用条件共同保护；无法证明匹配时原样保留原 SPIR-V。已有外部 SPIR-V 覆盖文件优先，C1 不覆盖它。项目不修改或复制外部 Sirit，不改 game/ 源文件。

启动记录为 `v3-r6-depth-reduction`，`v3_lighting_reduction=1/0` 是本轮候选差异。full 的 `lighting_optimization` 单独列出启动能力、每个 permutation 的 applied/reason 和最终 shader 操作计数；回退不冒充性能数据损坏。最终 `.spv` 在 C1 之后转储；`.irprogram.txt` 在它之前生成，因此新增规约需检查最终 SPIR-V。

固定存档来源仍为 `state/play/home`，复制到本次 session；V3/full 的保存变更不自动归并、不扫描历史 session。人工入口沿用项目共享缓存，full 继续用私有缓存与同一次运行中的采集工具。

本轮仅完成源码、回归源码、静态 review 和脚本语法检查；未代构建/执行测试/启动游戏。C1 尚未经过运行正确性和性能验收，不预告 FPS 增益。实施与验证命令见 `doc/36-c1-r6-depth-reduction.md`，A1 实测见 doc/35。

## 路径和用户数据

project.json 保存相对项目根目录的引擎、游戏及既有构建依赖路径。脚本按自身位置解析，从其他工作目录调用 BAT 不改变含义。引擎位置固定在本项目 Build/ 中，清单记录当前选定配置对应的 EXE、构建记录与 SHA-256；构建尚未完成时清单明确标记为未构建。修改清单也不能绕过项目边界和构建记录检查。系统工具通过现有 PATH、用户／系统环境及已安装目录定位，不安装依赖。

- profiles/：1080p、Pro 4K 正确性配置及六档分辨率所用补丁模板。
- state/baseline/：初始配置、输入、补丁来源和存档快照。
- state/play/home/：所有分辨率共享的正式游玩进度。
- state/play/user/：持久配置和输入；state/play/launcher.json 保存菜单偏好。
- state/sessions/：每次运行的隔离配置、存档、日志及控制记录。
- state/runtime/cache/、shader/：项目内共享缓存。

启动先取得项目运行锁，再准备会话；异常期间引擎尚未退出时继续保持锁。路径检查拒绝越界及未经创建的目录联接；只有会话中的 home/cache/shader 使用明确指向项目内部的联接。

普通游玩仅在退出码为零且原程序指纹未变时归并进度。存档和配置逐文件先完整写入同目录的临时文件，再原子替换旧文件；失败时保留旧文件和会话副本。临时占用最多为当前复制的单个文件大小。全局与游戏专属配置均在启动时按会话目录转换路径，持久化前转回项目根目录相对路径；指向项目外或未经允许目录联接的配置路径会被拒绝。崩溃会话不覆盖正式进度。

两种基线均保持精确回读、SDR、完整粒子及既有正确补丁。原 GUI 的 AppData 数据不再读取或修改。项目首次缓存预热可能产生编译卡顿。

game/、doc/、state/、out/、Build/、Install/ 及 .vs/ 等按 .gitignore 排除。profiles/ 仅允许提交 1080p.json、pro4k.json 和 CUSA00309.xml 三个公共运行模板；用户实际设置和存档继续保存在被忽略的 state/。运行所需的私有配置、游戏及开发阶段复用的外部依赖仍须由用户提供；引擎由本项目构建，不能只靠克隆仓库获得完整运行环境。

## 诊断采集

打开 `diagnostics` 文件夹，双击对应 BAT。所有专项模式及 `full.bat` 均自动识别继续存档、等待场景与预热、执行预设输入、触发采集、退出游戏；full 只启动一次游戏，在同一 PID、同一会话中集中采集全部已设计分析方向。首次 Windows 管理员授权仍需确认，之后无需操作游戏或终端。根目录 `diagnose.bat` 同样直接执行完整采集。

| 脚本（diagnostics 目录） | 分析方向与自动操作 |
| --- | --- |
| check.bat | 检查工具、OCR、脚本、控制协议及菜单决策，不启动游戏 |
| cpu.bat / vtune.bat | CPU、回读等待与热点；静止观察交替短暂转动视角 |
| gpu.bat / compute.bat / draw.bat | GPU 调度、计算／绘制；执行相同的视角及技能输入 |
| memory.bat / vtune-memory.bat | 分配、缓存与内存访问；重复视角及技能输入 |
| validation.bat / crash.bat / shaders.bat | 自动执行场景输入，收集校验、崩溃或着色器信息 |
| renderdoc.bat / nsight.bat | 自动触发图形帧捕获／GPU Trace，无需 F8 或 F11 |
| all.bat | 单轮综合调试，计算／绘制统计自动切换，并追踪两帧 guest 命令 |
| full.bat | 同一游戏进程集中开启综合调试、Nsight、VTune CPU/内存采集；RenderDoc 与 Nsight 互斥，默认使用 Nsight |

各采集入口先自动进行离线自检：检查中文 JSON、共享读取、文件占用时保留完整旧记录、截图写入完成及合成菜单的实际 Windows OCR，并使用模拟进程检查截图、按键、视角、退出指令及身份校验；在 state/offline-tests/ 和 state/sessions/offline-control-* 创建约数 KiB 临时文件并自动清理，不启动引擎。

full 从启动阶段记录 CPU/GPU 时间线和内存信息。Nsight 通过已安装的官方 SDK 在引擎内注入：先预加载验证层，再注入 Nsight，随后初始化 Vulkan，降低验证层过早销毁的风险。已复现 Nsight 的 nvperf_grfx_target.dll 在进程卸载时访问冲突；诊断退出现在先等待采集工具导出、完成引擎写盘，记录回执后结束自身进程，避开该第三方 DLL 卸载回调。此规避仅用于收到采集退出请求且工具完成的诊断会话，不代表第三方 DLL 本身已修复。控制器附加同一游戏 PID 并等待原生 SDK 触发；场景就绪并预热完成后才发起 GPU Trace。当前安装的 Nsight 注入会阻止 RenderDoc DLL 加载，因此二者每轮只能选择一个：diagnostics/settings.json 的 full_gpu_collector 默认是 nsight，也可改为 renderdoc；另一工具保留独立 BAT。选择写入本轮 plan.json，运行中不会切换。 Nsight 组合保留核心校验、同步校验和引擎 GPU 检查点；该组合排除 GPU 辅助着色器校验；外部崩溃拦截层保留在独立入口，与 Nsight 同时启用尚未验证，实际排除项写入 plan.json。GPU 辅助校验单独运行也已复现设备丢失，因此所有标准入口默认关闭 GPU 辅助着色器插桩，保留核心／同步校验；外部崩溃诊断可通过 crash/all 入口采集。diagnostics/settings.json 的 gpu_assisted_validation 可显式设为 true 以继续专项实验（Nsight 组合仍排除它）。这是对故障功能的隔离，尚未修复 GPUAV 本身，不能声称所有调试器同时开启。预设 PRO 4K，场景内自动操作窗口为 60 秒；最早在启动采集器 90 秒后开始场景操作，并且必须先取得继续存档、任务 HUD 或受控暂停／返回响应等证据，并结合渲染活动判断已进入场景，再预热 20 秒。采集副本临时使用英文菜单，便于 Windows OCR 识别。 自动采集副本的音量设为 0，音频解码仍运行，正式游戏音量不变。进入存档的等待上限为 240 秒，识别失败会保存失败状态并结束这一轮。计算、绘制和着色器模式也会自动追踪两帧 guest 命令。结果窗口显示后自动关闭，不要求回车。

各轮使用正式存档和配置的副本及私有缓存，保持精确回读、SDR、完整粒子及已审计补丁，数据保存到已忽略的 `state/sessions`，不归并至正式存档或配置。采集区为 state/sessions。每次诊断 BAT 或 run.bat --debug 实际启动前，在运行锁内检查该区总量；若超过 20 GB（20,000,000,000 字节），保留按目录时间前缀排序的最近一次 session，清理其余项目生成的 session，再创建本轮新 session。正好达到阈值不清理，唯一最近记录即使超过阈值也保留；检查、仅准备和普通游戏启动不触发清理。清理不跟随目录链接，发现活动游戏或采集器时停止清理；记录保存到 state/runtime/session-retention.json。非 session 目录不自动删除。

各模式均取消分项容量上限和会话总容量上限。VTune 和引擎日志使用不限量设置，WPR 使用文件模式持续记录；每约 5 秒统计本轮会话全部普通文件总量、已观测峰值及所在磁盘剩余空间，写入 diagnostics/storage.csv 和 status.json 的 usage 字段，等待工具结束或合并 ETL 时也继续监控。统计包括日志、VTune、WPR 临时文件、抓帧、着色器、私有缓存和存档副本，不跟随目录链接。仅当可用磁盘空间低于 2 GiB 保留值时因空间原因请求正常退出，保留已采集文件；此保留值不是会话容量上限。文件统计是逻辑长度，采样间隔及刷盘过程中的瞬时峰值可能未被观察，不能视为磁盘硬配额。

full 的 `diagnostics/coverage.json` 记录同一目标 PID、创建时间、采集窗口、各工具实际产物和缺失项；`diagnostics/automation.json` 保存识别截图、OCR 结果、输入请求、游戏端逐条执行回执和场景判据；自动场景判断为启发式证据，亮度、粒子及游戏语义没有自动验收。即使未进入场景，full 也会保留已取得的启动阶段跟踪；崩溃诊断输出保存到会话的 diagnostics/cdl。工具失败、缺少捕获文件或超时会记录失败／不完整结果；full 不重启游戏补采，也不改成多轮。命令采用 FIFO 队列，采集等待计划动作全部执行；游戏退出后才合并跟踪和解析数据。diagnostics/artifact-validation.json 记录已安装工具的解析结果：WPR 事件统计、VTune 完整后处理和摘要、Nsight 数值表或 RenderDoc 帧缩略图。采集完成状态与非致命运行时错误分开报告，原始错误保留在 runtime-errors.json；设备丢失、关键错误和数据缺失仍判定失败。诊断结果不用于 FPS 验收。手动控制 BAT 继续保留在 `diagnostics/control`，作为可选的中断与查看入口。

脚本先请求正常退出；若失去响应，会在等待后核对本轮进程路径、PID 和创建时间，再结束该诊断进程并标记失败，完整套件停止继续推进。详细说明见 `doc/04-diagnostics.md`（按约定忽略 Git）。

## 用户验证

以下命令由用户执行；代理不自动运行游戏、测试或构建：

    check.bat
    check.bat 720p
    check.bat 900p
    check.bat 1080p
    check.bat 1440p
    check.bat 1800p
    check.bat pro4k
    test.bat

check.bat 不带参数时核对保存的菜单分辨率，指定档位时核对对应配置；只核对引擎、游戏、配置及启用补丁原字节；test.bat 使用系统已有 Python，运行离线输入、显示预设、原子写入和测量边界回归测试，并执行 Windows PowerShell 的真实文件替换检查，不启动游戏。后者只在 state/offline-tests/ 创建约数 KiB 的临时文件，并自动清理。

run.bat --benchmark 1080p / run.bat --benchmark pro4k 使用基线存档副本，并仅将测试游戏音量设零。控制入口为 control.bat status / screenshot / quit；按项目清单、会话、进程路径、PID 和创建时间核对目标。

在实际游戏场景与缓存预热后记录测量窗口：

    measure.bat -StartSeconds 90 -EndSeconds 150 -SceneConfirmed -BrightnessConfirmed -ParticlesConfirmed -ResolutionConfirmed

逐行读取 guest flip CSV，要求记录覆盖整个指定窗口，再保留窗口内完整帧间隔；采集中断而未覆盖窗口时拒绝给出验收结果。没有画面、粒子及分辨率确认时不接受为基线；新增分辨率仅记录测量结果，不套用 1080p 或 Pro 4K 的验收门槛。它也不能证明显示面板扫描或全游戏稳定帧率。

analyze.bat 使用已有 Python/LLVM 只读分析原始游戏，输出限制在 doc/。build.bat --check 定位既有开发环境；build.bat 编译当前项目并在成功后切换启动引擎。后期 C++ 缓存回归入口位于 tests/cache_runtime/，复用项目清单中的依赖；Google Test 测试只使用已安装的 GTest。

分析依据见 doc/01-porting-evidence.md、02-infamous-optimization-candidates.md 和 03-baseline-and-debug.md。缓存遍历／回收、补丁边界及项目数据路径等 C++ 修正只有重新构建后才进入运行引擎；游戏验收由用户执行，不将构建成功作为性能成果。

origin：git@github.com:VeatyelpeaWQBi/inFamous_2nd_son_pc.git。提交和推送分别需要明确指令。


full 的 CPU 指令热点由 VTune 硬件采样负责，WPR IFSSConcurrent 保留线程调度／等待栈、GPU、内存和 I/O。已在无游戏对照中复现 VTune 活跃时 WPR Timer/TimerFixed 不再提供定时 CPU 样本，因此不再声称 full 包含这份重复采样。独立 cpu/all 模式继续保留 WPR 定时采样。每轮导出 vtune-gameplay-hotspots.csv/json，按游戏实际窗口和 PID 验证函数、CPU Time、模块及访存指标；缺少目标 CPU 热点仍判为不完整。时间关联受摘要秒级时钟限制，导出裁去两端各一秒，并记录误差。full 仍在同一游戏进程的一次运行中并行采集。

full 的内容验收写入 diagnostics/data-quality.json：核对 WPR 实际 GPU 内核事件、丢失计数和时间范围，帧记录的 UTC/QPC 时钟，EXE/PDB 指纹及 GUID/age，VTune 带宽校准和采集覆盖，以及传感器完整数据行。缺少必需数据时保留产物并报告不完整，不以文件非空代替内容验收。

GPU 连续调度时间线由 WPR 覆盖完整操作窗口；当前 Nsight Graphics CLI 对单次 GPU Trace 强制小于 10 秒，因此 Nsight 使用同一游戏进程内SDK 触发的连续 3 帧快照补充硬件指标（9 秒保护时长），不声称其硬件指标覆盖整个 60 秒。Nsight 和验证层的已验证加载顺序保留。帧记录附带精确 UTC 与 QPC 读取区间，可与 ETW 对时；它仍是 guest flip，不代表显示器实际扫描输出。GPU 传感器采用完整单次查询后写行，带 UTC/QPC，关闭时等待最后一次查询，不截断正在写入的 CSV。所有采集副本保持静音。

Nsight 自动导出的跨帧事件指标矩阵会随事件和帧数同时膨胀，完整模式保留连续 3 帧详细快照，GPU 的全窗口时间线仍由 WPR 采集。内容验收逐行读取大型表格，避免把 GB 级表一次性装入内存。

传感器的 UTC 使用查询进程真实退出时间，QPC 结束列是读取结果时的观察上界；验收优先使用保留下来的 NVML 原始时间及启动记录的 UTC 偏移对齐，防止结束阶段延迟读取被误认为晚发生的采样。验收也检查操作窗口内部是否存在超过 5 秒的遥测缺口。

## 关键路径计时

diagnostics/timing-baseline.bat、timing-critical.bat、timing-lighting.bat 使用同一优化内核和 Pro 4K 条件，分别关闭额外计时、开启 CPU 关联计时、增加指定光照着色器 GPU 计时。baseline 指采集对照，不代表关闭 V1／V2。三种入口均为手动游玩，F1 退出，配置／存档变更留在会话；没有外部采集器或 UAC。三者均设置 perf_log = 0，避免原有 1 ms 挂起线程采样混入比较；CPU 关联事件及指定 GPU 查询不依赖该旧采样器。

本轮涉及 C++，先运行 build.bat。建议同场景 baseline → critical → baseline，再录一轮 lighting；当前无需重录 full。退出后自动生成会话内的 timing-summary.json，默认分析最后 60 秒，并全量检查原始事件完整性。它区分 CPU 等待/写回/呈现队列和 GPU 查询区间，不能还原无采集时的每帧 GPU 成本，也不自动证明优化收益。

实现、字段、测量边界和用户验证命令见 doc/21-v2-critical-path-timing.md（doc/ 按约定忽略 Git）。新增纯内存回归：python -B tests/test_timing_analysis.py；可选原生队列回归目标 ifss_path_trace_queue_test。测试/构建/游戏均由用户执行。

V3 回读回归修复：CPU 需求回读保留 V2 原有窗口，不因粒子计数访问缩小到单页；显式启用批量预取但未记录任何复制时，自动回退 V2 预取。V3 人工／full 入口均关闭 `v3_demand_readback` 和 `v3_prefetch_batches`；R4 同时关闭其余旧候选及布局共享。引擎修改需要重新执行 `build.bat` 才能生效；本次未代执行构建或游戏测试。

V3 R4：新增证据仅在 `full-optimization-v3.bat` 开启；普通与人工入口保持 V1/V2 内核。R3 布局共享代码保留但默认关闭，其构造复用率不计作正常游戏收益。

## 原生辅助模块迁移

普通构建、启动菜单、设置、预检和退出已接入 scripts/native/ 的 C# 模块；现有 Windows PowerShell 负责流程调度。普通运行无需 Python。引擎源码未因本次迁移改变；请先 build.bat 更新来源记录，再 run.bat。性能分析与调试工具在独立提交中迁移。详见 scripts/native/README.md。
