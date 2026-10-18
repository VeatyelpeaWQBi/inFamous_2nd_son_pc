# inFAMOUS Second Son — V2 Release

Windows x86-64（CPU 支持 AVX2 / x86-64-v3，显卡驱动支持 Vulkan 1.3 以上）游戏专用引擎，基于 shadPS4/AYOUB1080p 的 GPL 源码，保留已验收的 V1/V2 内核优化。仅支持 CUSA00309 01.07 及项目已核对的 eboot.bin 指纹。

本分支移除 V3 候选、调试面板、CPU 采样线程、性能计数、GPU 计时抓帧、RenderDoc/Nsight/VTune 联动和自动测试操作。保留必要警告/错误、游戏时钟、同步/回读、着色器缓存、输入法/存档对话框、截图及退出确认。

## 运行

共享包解压后直接运行 inFAMOUS Second Son.exe 或 run.bat。不需要 Python、模拟器 GUI、编译器或调试工具。

编辑根目录 launcher.json：

    {
      "game_path": "game",
      "mode": "pro4k",
      "display": "borderless"
    }

- game_path：默认查找 game/eboot.bin 或 game/CUSA00309/eboot.bin，其他资源与程序一起放置。也可填写自己的游戏目录或 eboot.bin 路径；相对路径以包根目录为准，支持绝对路径。
- mode：1080p 或 pro4k。引擎自动选择互斥的分辨率补丁，保留精确回读、SDR、光照列表及曝光修复。
- display：fullscreen、window 或 borderless。
- F1 打开退出确认，Enter／手柄 A 确认，Esc／手柄 B 取消。Alt+F4 同样可退出。

可临时覆盖：

    run.bat --game "D:\\My Games\\CUSA00309" --mode 1080p

常规配置位于 state/play/user/config.json，存档固定为 state/play/home/，缓存位于 state/runtime/。不从 AppData 取配置或存档；不复制、扫描历史 session 来挑选存档。分辨率、显示模式由 launcher.json 控制；旧调试配置键在本分支不起作用。

游戏目录保持只读；XML 补丁只修改运行中的程序映像，继续核对原字节。包不附带游戏、个人存档、私有配置或录制数据。系统和显卡驱动由使用者提供。

## 从源码构建与打包

两套入口使用同一个 Release 内核及 Build/Release/，避免重复维护引擎；区别是运行库是否随包提供。

| 用途 | 构建 | 打包 |
| --- | --- | --- |
| 本机精简版：复用已有运行库 | build.bat | release.bat |
| 网友共享版：携带可分发运行库 | build-portable.bat | release-portable.bat |

源码修改后必须重新构建，运行不进行实时编译。先构建，再打包。打包不会自动构建或启动游戏。源码变化、构建失败、程序校验不符、依赖缺失会阻止打包。可复用同一次最新构建分别生成两种包。

- 编译复用现有 CMake、Ninja、Visual Studio Clang 与 project.json 中配置的第三方源码/FFmpeg，不安装或下载依赖。
- 共享打包使用已安装 LLVM 检查 EXE/DLL 的 x64 导入及递归依赖，携带所需第三方 DLL 和现有 VC Redistributable；缺失则报错。不复制 Windows 组件或显卡驱动。
- 本机版在 release-manifest.json 中列出需由本机环境提供的运行库；共享版记录相同清单并附 DLL。
- 两版均包含配置、分辨率补丁、许可证和逐文件 SHA256 清单，不含源码树/SDK/编译器/调试器/PDB/session/存档。
- 输出保存在 out/release/local-时间-编号/ 或 out/release/portable-时间-编号/ 及同名 ZIP，不覆盖旧包。
- run.bat 只调用包内 EXE 或当前 Build/Release/，不会回退到旧 RelWithDebInfo 或其他项目引擎。

首次构建的中间文件通常占 1～3 GiB。发布脚本只在用户执行时创建包。引擎运行、构建和打包使用同一项目锁，防止同时改写程序或共用存档/配置。

本次源码清理不等于已完成游戏验收；构建及 1080p/Pro 4K 的亮度、粒子、存档和性能验证仍需执行。

## 来源与许可

保留 shadPS4 Emulator Project、AYOUB1080p 及其他源文件中的版权和 GPL-2.0-or-later 声明，见 LICENSE、LICENSES。第三方许可随包位于 third-party-licenses。项目源代码：[VeatyelpeaWQBi/inFamous_2nd_son_pc](https://github.com/VeatyelpeaWQBi/inFamous_2nd_son_pc)。
