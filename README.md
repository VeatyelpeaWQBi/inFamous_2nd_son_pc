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

## 运行库安装参考 / Runtime installation reference

### 中文

以下对应关系依据 2026-10-19 的实际发布包核对。项目引擎为 **V2 Release**，Windows EXE 文件版本为 **0.18.1.0**；游戏的 `APP_VER`（例如 01.07）与运行库版本无关。不同构建可能使用相同 EXE 文件版本，请同时核对包内 `release-manifest.json` 的 `runtime_sha256`；下表列出其前 12 位。

| 引擎与构建 | 发布包 | EXE SHA256 前缀 | VC++ 运行库安装参考 |
| --- | --- | --- | --- |
| V2 Release / EXE 0.18.1.0 / 2026-10-19 | `lite-20261019-160236-50f7f1.zip` | `5ddee942e856` | 安装 Microsoft Visual C++ v14 Redistributable **x64**；参考基线 **14.51.36247.0 或更新版本** |
| V2 Release / EXE 0.18.1.0 / 2026-10-19 | `portable-20261019-154150-11b0ec.zip` | `d6428b929785` | 已携带以下四个 x64 DLL，完整解压后通常无需另装 VC++ 运行库 |

| 当前构建使用的 DLL | 文件版本 | lite | portable |
| --- | --- | --- | --- |
| `MSVCP140.dll` | `14.51.36247.0` | 由系统运行库提供 | 包内附带 |
| `MSVCP140_ATOMIC_WAIT.dll` | `14.51.36247.0` | 由系统运行库提供 | 包内附带 |
| `VCRUNTIME140.dll` | `14.51.36247.0` | 由系统运行库提供 | 包内附带 |
| `VCRUNTIME140_1.dll` | `14.51.36247.0` | 由系统运行库提供 | 包内附带 |

`14.51.36247.0` 是本次实际选用的运行库及建议安装基线，未将更旧版本逐一验证为最低可用版本。后续构建更换工具链或运行库时，应重新核对并更新对应记录，不能将本表自动套用到所有使用 `0.18.1.0` 版本资源的 EXE。

- **lite 安装方式**：下载并安装微软官方 [Visual C++ v14 x64 运行库](https://aka.ms/vc14/vc_redist.x64.exe)，确认版本达到上表参考基线。仅安装 x86 版本不能满足本引擎需求。微软说明运行库架构需匹配程序，版本应不早于构建工具所需版本，见 [官方版本与兼容说明](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170)。
- **portable 使用方式**：保持上述四个 DLL 与 `inFAMOUS Second Son.exe` 在同一目录；若提示缺少其中某个文件，先重新完整解压发布包。
- **两版均需 Vulkan 图形环境**：显卡与驱动至少支持 **Vulkan 1.3**，并满足引擎要求的扩展和功能；单有 `vulkan-1.dll` 不代表设备支持完整。按显卡型号安装或更新官方驱动：[NVIDIA](https://www.nvidia.com/en-us/drivers/)、[AMD](https://www.amd.com/en/support/download/drivers.html)、[Intel](https://www.intel.com/content/www/us/en/download-center/home.html)。Loader 与实际设备驱动的区别见 [Khronos 官方说明](https://docs.vulkan.org/guide/latest/checking_for_support.html)。一般游玩无需安装 Vulkan SDK。
- **已内置的依赖**：FFmpeg、SDL、OpenAL、FreeType 等已静态链接到此构建的 EXE，无需另外安装这些库或 FFmpeg 命令行程序。Python、Java、Visual Studio、CMake、RenderDoc、Nsight、VTune 均不是此发布包的游玩依赖。
- **平台条件**：Windows x64、支持 x86-64-v3 / AVX2 的 CPU，以及上述显卡功能。当前不提供统一的自动依赖安装向导；运行库安装不能弥补硬件指令集或 GPU 功能缺失。

### English

The table above applies to the inspected **2026-10-19 V2 Release** packages, whose EXE file version is **0.18.1.0**. This is independent of the game's `APP_VER`. Match the package and the EXE hash in `release-manifest.json`; the same file version can identify different builds.

- **lite** (`5ddee942e856…`): install [Microsoft Visual C++ v14 Redistributable x64](https://aka.ms/vc14/vc_redist.x64.exe). **14.51.36247.0 or newer** is the installation baseline for this build, not a tested minimum across all older runtime versions. The x86 package alone is insufficient. See [Microsoft's architecture and version requirements](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170).
- **portable** (`d6428b929785…`): includes the four DLLs listed above, all version **14.51.36247.0**. Keep them beside the EXE; a separate VC++ installation is normally unnecessary.
- **Both editions** require a compatible x64 Windows environment, an x86-64-v3 / AVX2 CPU, and a GPU driver providing Vulkan **1.3+** and the engine's required features/extensions. Use the official NVIDIA, AMD or Intel driver links above. A Vulkan loader alone does not establish device compatibility; see [Khronos](https://docs.vulkan.org/guide/latest/checking_for_support.html). The Vulkan SDK is not required for normal play.
- FFmpeg, SDL, OpenAL, FreeType and other static dependencies are already linked into this EXE. No separate FFmpeg installation, compiler or profiling tools are required. Recheck this mapping for future builds; this release currently has no unified automatic dependency installer.

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
| 精简版（lite）：复用已有运行库 | build-lite.bat | release-lite.bat |
| 网友共享版（portable）：携带可分发运行库 | build-portable.bat | release-portable.bat |

源码修改后必须重新构建，运行不进行实时编译。先构建，再打包。打包不会自动构建或启动游戏。源码变化、构建失败、程序校验不符、依赖缺失会阻止打包。可复用同一次最新构建分别生成两种包。

- 编译复用现有 CMake、Ninja、Visual Studio Clang 与 project.json 中配置的第三方源码/FFmpeg，不安装或下载依赖。
- 配置会由 cmake/DependencyCompatibility.cmake 对已配置的共用依赖应用可重复的编译兼容修正：CMake 策略、编译器参数、Windows 版本查询、宏、头文件及等价的二进制常量写法。开发分支共用该目录时也会看到这些修正；不升级或复制依赖，不改变压缩/加密算法和数据布局。已修正的内容不重复写入，未知源码会停止并要求检查。
- cmake/DependencyDiagnostics.cmake 仅为 protobuf/OpenAL 的条件支持字段偏移、LibreSSL 单个旧 BIO 回调源文件保留明确的 ABI 扩展诊断规则，原因逐项记录在该文件。没有全局关闭警告，引擎自身及其他过时接口、类型和布局诊断仍然启用。
- 两种构建入口在交互控制台单行刷新 `[已完成/总数] 文件.cpp`，警告和错误完整逐行保留；输出被重定向时使用普通逐行输出。工具连续 15 秒没有新输出时显示运行状态。完整原始输出保存到 Build/Release/build.log，每次构建覆盖上一份，失败会标明所在阶段。Ninja 遇错后继续检查其他独立文件，集中报告错误并最终返回失败；已完成的编译产物仍可复用，无须删除 Build/ 重来。
- 共享打包使用已安装 LLVM 检查 EXE/DLL 的 x64 导入及递归依赖，携带所需第三方 DLL 和现有 VC Redistributable；缺失则报错。不复制 Windows 组件或显卡驱动。
- 精简版（lite）在 release-manifest.json 中列出需由运行电脑提供的运行库；共享版记录相同清单并附 DLL。
- 两版均生成默认指向 game/ 的干净配置，包含分辨率补丁、许可证和逐文件 SHA256 清单，不含源码树/SDK/编译器/调试器/PDB/session/存档。
- 输出保存在 out/release/lite-时间-编号/ 或 out/release/portable-时间-编号/ 及同名 ZIP，不覆盖旧包。
- run.bat 只调用包内 EXE 或当前 Build/Release/，不会回退到旧 RelWithDebInfo 或其他项目引擎。

首次构建的中间文件通常占 1～3 GiB。发布脚本只在用户执行时创建包。引擎运行、构建和打包使用同一项目锁，防止同时改写程序或共用存档/配置。

本次源码清理不等于已完成游戏验收；构建及 1080p/Pro 4K 的亮度、粒子、存档和性能验证仍需执行。

## 来源与许可

保留 shadPS4 Emulator Project、AYOUB1080p 及其他源文件中的版权和 GPL-2.0-or-later 声明，见 LICENSE、LICENSES。第三方许可随包位于 third-party-licenses。项目源代码：[VeatyelpeaWQBi/inFamous_2nd_son_pc](https://github.com/VeatyelpeaWQBi/inFamous_2nd_son_pc)。

兼容性策略的独立离线检查由用户执行，不启动游戏：

    cmake --build Build/Release --target ifss_second_son_compatibility_test
    .\Build\Release\ifss_second_son_compatibility_test.exe
