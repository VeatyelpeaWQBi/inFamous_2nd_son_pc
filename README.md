# inFamous_2nd_son_pc

Windows x86-64 / inFAMOUS Second Son 港版 CUSA00309 01.07 的独立适配项目，当前处于 BAT 阶段。

源码保留 shadPS4 的版权和 GPL 许可。BAT 调用已核对的现有引擎，不生成、复制或修改 EXE；原始游戏文件只读。源码优化需后期获得构建授权才会进入新引擎，当前运行引擎与本项目源码不宣称版本等价。

## 运行

唯一游戏启动入口为 run.bat，双击后在菜单选择分辨率、语言、确认按键、帧数显示，以及全屏／窗口／无边框窗口。选择保存在 state/play/launcher.json，下次启动继续使用。旧设置保留，首次增加分辨率时继承最近保存的游戏配置，缺少历史配置时默认 1080p。

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

窗口标题、任务栏图标及启动／退出菜单使用游戏名称和图标。隐藏的窗口辅助进程只修改经路径、PID 和创建时间核对的 SDL 窗口属性，不读写游戏内存。BAT 阶段任务管理器中的 EXE 文件名和文件描述沿用原引擎。

## 启动模块与运行环境

启动菜单、退出窗口、配置／补丁预检、帧统计和静态分析均由 `scripts/native/` 中的 C# 模块实现，窗口使用 Windows Forms。现有 BAT／PowerShell 仅负责入口、会话与进程编排；不再调用 Python 或 Tkinter。

模块由 Windows x64 PowerShell 使用系统 .NET Framework 的 C# 编译器加载到内存，不要求安装 .NET SDK、Python 或新工具链。首次调用仍会编译辅助模块，编译临时目录固定为 `state/runtime/native-helper-temp/`；不生成或替换游戏引擎 EXE。C++ 引擎源码改变仍需独立编译。

菜单继续读取／保存 `state/play/launcher.json`（schema 3），沿用六档分辨率、语言、确认按键、FPS、三种显示模式，以及原有配置／补丁守卫。窗口辅助模块仅访问核对过 EXE 路径和创建时间的目标进程。帧统计还要求记录覆盖完整测量窗口，避免截断数据被当作有效收益。

离线检查入口仍为 `test.bat`，使用合成二进制、CSV 与隔离配置，不启动游戏、不读取真实存档。首次辅助编译会生成临时文件；回归夹具仅写入 `state/offline-tests/` 并在安全检查后清理。

模块边界、调用协议及开发／Release 分支的合并注意事项见 [原生启动模块说明](scripts/native/README.md)。其他两个分支各自的入口、采集和多版本策略需要按该说明接入，不能用 main 的旧启动脚本覆盖它们。

## 路径和用户数据

project.json 保存相对项目根目录的引擎、游戏及既有构建依赖路径。脚本按自身位置解析，从其他工作目录调用 BAT 不改变含义。当前引擎位置为 ../../game/shadPS4QtLauncher/versions/shadps4-local-infamous.exe；SHA-256 固定，文件变化时拒绝启动。系统工具通过现有 PATH、用户／系统环境及已安装目录定位，不安装依赖。

- profiles/：1080p、Pro 4K 正确性配置及六档分辨率所用补丁模板。
- state/baseline/：初始配置、输入、补丁来源和存档快照。
- state/play/home/：所有分辨率共享的正式游玩进度。
- state/play/user/：持久配置和输入；state/play/launcher.json 保存菜单偏好。
- state/sessions/：每次运行的隔离配置、存档、日志及控制记录。
- state/runtime/cache/、shader/：项目内共享缓存。

启动先取得项目运行锁，再准备会话；异常期间引擎尚未退出时继续保持锁。路径检查拒绝越界及未经创建的目录联接；只有会话中的 home/cache/shader 使用明确指向项目内部的联接。

普通游玩仅在退出码为零且原程序指纹未变时归并进度。存档和配置逐文件先完整写入同目录的临时文件，再原子替换旧文件；失败时保留旧文件和会话副本。临时占用最多为当前复制的单个文件大小。配置持久化前转回项目根目录相对路径。崩溃会话不覆盖正式进度。

两种基线均保持精确回读、SDR、完整粒子及既有正确补丁。原 GUI 的 AppData 数据不再读取或修改。项目首次缓存预热可能产生编译卡顿。

game/、doc/、state/、out/、Build/、Install/ 及 .vs/ 等按 .gitignore 排除。profiles/ 仅允许提交 1080p.json、pro4k.json 和 CUSA00309.xml 三个公共运行模板；用户实际设置和存档继续保存在被忽略的 state/。运行所需的私有配置、游戏及清单指定的外部引擎仍须由用户提供，不能只靠克隆仓库获得。

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

check.bat 不带参数时核对保存的菜单分辨率，指定档位时核对对应配置；只核对引擎、游戏、配置及启用补丁原字节；test.bat 使用系统 .NET 执行 C# 离线回归，核对补丁冲突／原字节、SELF/ELF/SFO、显示预设、原子写入和测量边界，并执行 Windows PowerShell 的真实文件替换检查及脚本入口回归（JSON 数组、路径引号、返回数据和退出码），不启动游戏或显示菜单。后者只在 state/offline-tests/ 创建约数 KiB 的临时文件，并自动清理。

run.bat --benchmark 1080p / run.bat --benchmark pro4k 使用基线存档副本，并仅将测试游戏音量设零。控制入口为 control.bat status / screenshot / quit；按项目清单、会话、进程路径、PID 和创建时间核对目标。

在实际游戏场景与缓存预热后记录测量窗口：

    measure.bat -StartSeconds 90 -EndSeconds 150 -SceneConfirmed -BrightnessConfirmed -ParticlesConfirmed -ResolutionConfirmed

逐行读取 guest flip CSV，仅保留窗口内帧间隔。没有画面、粒子及分辨率确认时不接受为基线；新增分辨率仅记录测量结果，不套用 1080p 或 Pro 4K 的验收门槛。它也不能证明显示面板扫描或全游戏稳定帧率。

analyze.bat 使用 C# 及可选的已有 LLVM 只读分析原始游戏，输出限制在 doc/。build.bat --check 定位既有开发环境；独立 EXE 编译继续推迟。后期 C++ 缓存回归入口位于 tests/cache_runtime/，复用项目清单中的依赖；Google Test 测试只使用已安装的 GTest。

分析依据见 doc/01-porting-evidence.md、02-infamous-optimization-candidates.md 和 03-baseline-and-debug.md。缓存遍历／回收、补丁边界及项目数据路径的源码修正尚未编译验收，不作为当前引擎的性能成果。

origin：git@github.com:VeatyelpeaWQBi/inFamous_2nd_son_pc.git。提交和推送分别需要明确指令。
