# Native runtime and diagnostic modules

The normal-runtime commit provides the Windows Forms launch/exit menus, saved preferences, process/window identity, verified game patches, and native build receipts. It uses existing Windows PowerShell/.NET Framework; no Python or .NET SDK is needed for ordinary build/start/quit operations.

The C++ engine remains unchanged by this migration. Run `build.bat` after applying the migration to regenerate the source-bound build receipt, then `run.bat`. No external engine fallback is permitted. Tests use synthetic data only.

`NativeCommand.cs` contains runtime commands only. Its optional partial diagnostic hook has no implementation in a runtime-only build. Performance modules belong in `diagnostics/` and are added by the separate diagnostic migration commit. The runtime entry script never accepts diagnostic commands.

A Release package may compile only ProjectPaths.cs, LauncherSettings.cs, GameIdentity.cs and LaunchMenu.cs plus its small EXE entry adapter. Do not restore the development BAT/session/controller scripts, fixed-title Preflight, build provenance adapter or diagnostic tools into Release runtime packages. Keep the Release game's existing compatibility checks and configuration application.

Legacy Python files still used by developer analysis/tests are retired in the separate diagnostic commit. They are not invoked by the migrated normal runtime. The final development tree must contain no Python implementation or invocation.
