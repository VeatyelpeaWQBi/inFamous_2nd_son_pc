# Project-local dependency archive

All build inputs live under this project. `externals/` contains the restored
sources, nested dependencies, licenses, and `prebuilt/ffmpeg/lib/`. It contains
ordinary files rather than links to another checkout. System compilers, the
Windows SDK and installed diagnostic tools remain system prerequisites.

`dependencies.lock.json` pins every source commit and downloaded archive hash.
The ten support files under `bootstrap/` retain their original copyright and
license notices. They preserve the dependency build definitions, GCN headers,
RenderDoc API and stb headers without needing the original project directory.

Back up the whole project including its ignored `externals/` folder. A Git-only
copy must restore the archived inputs explicitly with the system Windows
PowerShell and `scripts/restore_dependencies.ps1`; this requires consent for
network access and disk use. The script uses this project's lock and bootstrap
files. It does not read another project's directory or install development tools.

`build.bat`, `run.bat` and `test.bat` never invoke dependency restoration or
download replacements. A missing, incomplete or modified archive stops the build.

The six FFmpeg libraries are from ext-ffmpeg-core commit
`94dde08c8a9e4271a93a2a7e4159e9fb05d30c0a`, release asset
`ffmpeg-windows-x64.zip`, SHA256
`5038087e7798a0b129dbb4ff4a0caf4b6995aff91f5bf424096f65840385661d`.
The source headers and license files are retained under `externals/ffmpeg-core/`.

Current archive: 54 pinned modules, 41,489 source/library files, approximately
879 MiB (1.15 GiB including retained downloads/metadata). An inventory is stored
in `externals/dependency-inventory.tsv` and its SHA256 is bound by the lock.
Internal symbolic links are materialized as ordinary files/directories. Upstream
dangling link blobs retain Git for Windows' ordinary-file representation; link
conversion records are retained alongside the downloaded source archives.
