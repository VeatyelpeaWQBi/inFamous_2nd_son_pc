# 原生启动模块 / Native launcher modules

本目录为 main 迁移后的 C# 共享实现。只使用 Windows x64 PowerShell、系统 .NET Framework、Windows Forms 与 Windows API；不需要 Python/Tkinter、NuGet 或 .NET SDK。用户已明确授权本轮使用 C++/C#，这不是对后续新增技术栈的默认授权。

## 模块对应

| 原模块 | 共享实现 | 职责 |
| --- | --- | --- |
| project_paths.py | ProjectPaths.cs | 路径／链接检查、UTF-8 JSON、限于 state/doc 的原子写入 |
| game_image.py | GameImage.cs | 有界 SELF/ELF、零填充映射、SFO、FNV-1a |
| launcher_settings.py | LauncherSettings.cs | 六档分辨率、语言资源、schema 3 偏好与显示配置 |
| game_identity.py | GameIdentity.cs | 名称、图标、STA UI 与 Windows 参数转义 |
| launch_menu.py | LaunchMenu.cs | Windows Forms 启动设置 |
| exit_menu.py | ExitMenu.cs | 外部退出确认，复用所属分支的 control.ps1 |
| window_branding.py | WindowBranding.cs | 进程路径／创建时间核验、窗口标题／图标／最大化 |
| preflight.py | Preflight.cs | 引擎／游戏指纹、标题版本、精确回读／SDR、补丁原字节及重叠检查 |
| metrics.py | Metrics.cs | 流式 CSV、完整采样窗口、均值 FPS 与 nearest-rank P95/P99 |
| analyze_game.py | AnalyzeGame.cs | 指纹约束的 18 个有界反汇编窗口、可选现有 LLVM |
| tests/bootstrap/test_bootstrap.py | tests/bootstrap/test_native.cs | 合成输入离线回归，无外部测试框架 |

`NativeCommand.cs` 提供统一的无状态调用入口；`../native_tools.ps1` 是加载／调用层，`../native.ps1` 是独立子进程入口。命名空间统一为 `Ifss.Native`，不会与开发分支的 `Ifss.ProcessIdentity` 冲突。

## 调用协议

已加载 PowerShell 共享层后，可调用：

```powershell
Invoke-ProjectNativeTool 'launch-menu' @('--mode','auto')
Invoke-ProjectNativeTool 'preflight' @('--mode','1080p')
Invoke-ProjectNativeTool 'preflight' @('--mode','pro4k','--session','state/sessions/example')
Invoke-ProjectNativeTool 'metrics' @('state/sessions/example/frames.csv','--start','90','--end','150','--mode','pro4k')
Invoke-ProjectNativeTool 'analyze-game' @('--output','doc/binary-analysis')
Invoke-ProjectNativeTool 'exit-menu'
```

启动菜单标准输出仍为分辨率 key；成功返回 0、取消返回 1、错误返回 2。其他模块成功返回 0、错误返回 1。PowerShell 调用层更新 `$LASTEXITCODE`，成功结果通过管道返回单个字符串，错误写入标准错误。预检和测量保留原 JSON 字段及 schema；不能把辅助工具成功当作游戏／图像／性能验收。

无 PowerShell 调用层的 C# 宿主可直接调用 `Command.Run(projectRoot, module, arguments)`。该 API 返回 `CommandResult`，不启动游戏。UI、设置、预检和统计也可直接调用各自公开类型，不依赖 main 的启动脚本。

独立后台窗口助手用现有系统 PowerShell 执行：

```powershell
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File scripts/native.ps1 `
    -Module window-branding -ArgumentsJson '["--session","state/sessions/example"]'
```

命令中的 example 只是协议示例，不能直接用于实际游戏。辅助 C# 在每个宿主进程首次调用时由系统编译器加载；临时目录位于项目 `state/runtime/native-helper-temp/`。没有持久引擎 EXE 构建或下载安装操作。

## 合并到 feature/fps-optimization

1. 保留本目录及两个 native PowerShell 文件的路径、命名空间与公开接口；不得把模块内容复制进现有入口函数。
2. 保留开发分支自己的 launch/build/diagnostic 流程。对共同的菜单、预检、分析、测量、退出调用，采用本轮 main 的 `Invoke-ProjectNativeTool` 连接方式；保留该分支诊断、V3、计时、存档与身份校验逻辑。
3. 退出菜单继续调用该分支的 `control.ps1`；不能把开发分支的 FIFO 控制队列替换成 main 的旧 command.json 协议。
4. 开发分支新增的 build_identity.py、analyze_timing.py、diagnostic_quality.py、diagnostic_window.py 及对应 Python 测试不在 main 本轮迁移范围。它们须单独迁移／适配，不能在合并后声称整个开发分支已经没有 Python。
5. 开发分支的构建来源校验比 main 更严格。接入新的 Preflight 时，必须保留其 source/build receipt 核验；不能仅用主程序 SHA256 检查替代构建来源检查。

## 合并到 release/optimization-v2

1. 只接入共享模块与所需适配，不恢复 main 的历史外部引擎路径、固定 CUSA00309 会话流程、run.bat 或调试／采集系统。
2. EXE 仍作为唯一入口。需由 Release 自己的入口打开共享菜单，再将结果应用于现有 Release 配置／补丁流程；本轮 main 修改不代表 Release 已完成该入口接线。
3. 菜单可通过 `LaunchMenu.Show(paths, mode, resolvedGameDirectory)` 或 launch-menu 的 `--game` 参数接收 Release 已解析的游戏目录。语言枚举不必固定为 game/CUSA00309；传入此目录时，没有 main 的基线配置也能加载默认偏好，已有偏好仍优先。
4. 偏好仍使用 state/play/launcher.json，字段 resolution、console_language、is_circle_enter、show_fps、display_mode。Release 的 window 与该偏好中的 windowed 应在入口适配，不能混改共享 schema。所选分辨率必须同时对应窗口尺寸、Pro 模式和互斥补丁。
5. Release 当前的标题／更新层／完整程序指纹检查、未核验版本降级策略、精确回读、SDR 与 V1/V2 内核逻辑必须保持。不要调用 main 的旧固定版本 Preflight 去替换这些策略。
6. 打包必须携带实际使用的共享模块与加载层，或在显式构建流程中编译并分发对应程序集；两版均不能再要求 Python。FPS 等菜单项须连接实际引擎功能，不能显示无效选项。

新目录与两个目标分支的既有文件无同名冲突，但入口架构本身不同，不能保证不经接线和验收就能直接运行。合并应核对实际冲突，而非用整份 main 脚本覆盖目标分支。本轮没有提交、合并或修改其他分支。

## 用户验证

```powershell
.\test.bat
.\check.bat 1080p
.\check.bat pro4k
.\run.bat
```

test 包含 C# 离线回归、PowerShell 文件替换检查及真实脚本入口检查，覆盖 Windows PowerShell 5.1 的 JSON 数组传递。只用合成二进制／CSV／配置及项目内隔离夹具；check 只读真实游戏并核对配置、指纹与补丁；run 会启动实际游戏。游戏亮度、完整粒子、显示模式、语言、键鼠／手柄和保存／退出仍需人工验证。
