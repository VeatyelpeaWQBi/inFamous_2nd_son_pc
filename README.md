# inFAMOUS Second Son — V2 Release

Windows x86-64（CPU 支持 AVX2 / x86-64-v3，显卡驱动支持 Vulkan 1.3 以上）游戏专用引擎，基于 shadPS4/AYOUB1080p 的 GPL 源码，保留已验收的 V1/V2 内核优化。仅接受 Second Son 的 CUSA00004、CUSA00223、CUSA00305、CUSA00309，支持按地区及更新版本选择运行配置；不接受 First Light 或其他 PS4 游戏。

本分支移除 V3 候选、调试面板、CPU 采样线程、性能计数、GPU 计时抓帧、RenderDoc/Nsight/VTune 联动和自动测试操作。保留必要警告/错误、游戏时钟、同步/回读、着色器缓存、输入法/存档对话框、截图及退出确认。

## 运行

共享包解压后直接运行 inFAMOUS Second Son.exe 或 run.bat。不需要 Python、模拟器 GUI、编译器或调试工具。

编辑根目录 launcher.json：

    {
      "game_path": "game",
      "mode": "pro4k",
      "display": "borderless"
    }

- game_path：默认查找 game/eboot.bin 或 game/ 下四个地区编号目录中的 eboot.bin，其他资源与程序一起放置。若存在多个游戏副本，明确指定其中一个目录或 eboot.bin；相对路径以包根目录为准，支持绝对路径。
- mode：1080p 或 pro4k。匹配已核验程序时，自动选择互斥分辨率补丁并保留光照列表、曝光及防冻结修复。其他构建自动使用基础 PS4 1080p，关闭 Pro 模式及全部 XML 功能补丁，并输出一次明确提示；精确回读、SDR 和 V1/V2 内核逻辑继续保留。
- display：fullscreen、window 或 borderless。
- F1 打开退出确认，Enter／手柄 A 确认，Esc／手柄 B 取消。Alt+F4 同样可退出。

可临时覆盖：

    run.bat --game "D:\My Games\CUSA00309" --mode 1080p

常规配置位于 state/play/user/config.json，存档固定为 state/play/home/，缓存位于 state/runtime/。不从 AppData 取配置或存档；不复制、扫描历史 session 来挑选存档。分辨率、显示模式由 launcher.json 控制；旧调试配置键在本分支不起作用。

游戏目录保持只读；XML 补丁只修改运行中的程序映像，继续核对原字节。包不附带游戏、个人存档、私有配置或录制数据。系统和显卡驱动由使用者提供。

## 多地区与更新版本

地区编号来自 [shadPS4 上游 Second Son 补丁](https://github.com/shadps4-emu/ps4_cheats/blob/main/PATCHES/InfamousSecondSon.xml)。
程序使用最终挂载的 param.sfo 和 eboot.bin 识别游戏，包含自动加载的更新层；不能只凭目录名选择补丁。

| 构建 | 路径 |
| --- | --- |
| 港版 01.07，程序 SHA256 与现有核验指纹一致 | 保留现有 1080p / Pro 4K 全套补丁 |
| 其他已列地区的 01.07，完整程序指纹与上述相同 | 可使用同一补丁配置；仅在程序完全一致时复用地址，仍需该地区人工验收 |
| 四个地区中其他更新版本或不同指纹 | 基础 PS4 1080p，无固定地址补丁；可启动验证，运行正确性、性能和旧存档兼容性仍待实测 |
| First Light、其他 PS4 游戏 | 拒绝启动 |

profiles/second-son-builds.json 管理地区、更新版本、程序 SHA256 与补丁 XML 的映射。
新增经核验构建时增加独立记录和带原字节/哈希检查的 XML；不需要改动 V1/V2 引擎代码。
未核验构建不会继承同编号其他更新版本的地址，也不从 AppData 或历史用户补丁自动加载。
原港版已预热的 V2 缓存继续保留；其他构建的缓存按地区、更新版本及完整程序指纹隔离在 state/runtime/versions/，避免不同更新版复用不相符的预编译数据。
补丁整组验证通过后才写入，缺少守卫或地址重叠会在写入前停止。

存档根仍为 state/play/home/，游戏按地区编号保留各自目录，不自动复制地区间存档。同编号更新版本沿用游戏原有存档逻辑，不进行存档格式转换。

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
- 两版均生成默认指向 game/ 的干净配置，包含分辨率补丁、许可证和逐文件 SHA256 清单，不含源码树/SDK/编译器/调试器/PDB/session/存档。
- 输出保存在 out/release/local-时间-编号/ 或 out/release/portable-时间-编号/ 及同名 ZIP，不覆盖旧包。
- run.bat 只调用包内 EXE 或当前 Build/Release/，不会回退到旧 RelWithDebInfo 或其他项目引擎。

首次构建的中间文件通常占 1～3 GiB。发布脚本只在用户执行时创建包。引擎运行、构建和打包使用同一项目锁，防止同时改写程序或共用存档/配置。

本次源码清理不等于已完成游戏验收；构建及 1080p/Pro 4K 的亮度、粒子、存档和性能验证仍需执行。

## 来源与许可

保留 shadPS4 Emulator Project、AYOUB1080p 及其他源文件中的版权和 GPL-2.0-or-later 声明，见 LICENSE、LICENSES。第三方许可随包位于 third-party-licenses。项目源代码：[VeatyelpeaWQBi/inFamous_2nd_son_pc](https://github.com/VeatyelpeaWQBi/inFamous_2nd_son_pc)。

兼容性策略的独立离线检查由用户执行，不启动游戏：

    cmake --build Build/Release --target ifss_second_son_compatibility_test
    .\Build\Release\ifss_second_son_compatibility_test.exe
