# Release launch menu

The menu shares the development branch's scripts/native/ layout and C# settings,
paths and Windows Forms implementation. ReleaseEntry.cs is the EXE adapter;
IFSS_RELEASE only changes help text. The shared FPS checkbox is available in both branches;
Release applies it to its lightweight game-frame counter and supports the F10 toggle.
The C++ engine opens support/Ifss.Launcher.exe, receives the selected settings,
then applies its own mounted-game fingerprint and guarded-patch checks.

No Python, development startup/controller/test scripts or diagnostic modules are
included. The build compiles these five modules with the installed Windows x64
.NET Framework compiler; runtime requires the system .NET Framework, not an SDK.
The helper finishes before game loading, so it has no per-frame monitoring cost.
On Start it grants foreground activation to the specific engine PID before closing.
The engine raises its SDL window once after the display-mode transition. This handoff
adds no global focus permission, input hook or repeated focus-forcing loop.
User settings remain in state/play/launcher.json; game data is only read.