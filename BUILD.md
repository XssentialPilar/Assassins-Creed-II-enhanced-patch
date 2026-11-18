# Build and test

Use Windows with Python 3, the [LLVM-MinGW UCRT toolchain](https://github.com/mstorsjo/llvm-mingw) on `PATH`, and the i686 folder from the [SDL 3.4.16 MinGW development package](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.16). The initial release used LLVM-MinGW 20261020. The video shader build uses Windows' `d3dcompiler_47.dll`.

Build the controller artwork with [resvg](https://github.com/linebender/resvg/releases) and Python's Pillow. Put `resvg.exe` on `PATH` or pass `--resvg` with the executable path. Both are build tools.

```powershell
$sdl = 'C:\SDKs\SDL3-3.4.16\i686-w64-mingw32'
python tools/build_d3d9_probe.py --native-view --fit-video --high-refresh --lean-trace --controller-sdk $sdl

$build = 'build/native-view-video-fit-high-refresh-lean-trace-controllers'
foreach ($name in 'display', 'view', 'video', 'controller') {
    & "$build/ac2_${name}_smoke.exe"
    if ($LASTEXITCODE -ne 0) { throw "Failed test $name" }
}

$version = '1.1.0'
$tiles = New-Item -ItemType Directory -Path build/prompt-atlas -Force
& "$build/ac2_controller_smoke.exe" --render-atlas $tiles.FullName
if ($LASTEXITCODE -ne 0) { throw 'Failed to export prompt artwork.' }
python tools/render_prompt_atlas.py $tiles.FullName "dist/input-prompts-v$version.png" --version $version
python tools/package_release.py $version --build-dir $build --prompt-preview "dist/input-prompts-v$version.png"
```

Close any exclusive full-screen application if the Direct3D tests cannot create their test device. Tests exercise Direct3D forwarding, aspect-ratio geometry, video fitting, remapped keyboard and mouse prompts, controller glyphs, virtual input and rumble, DualSense trigger reports, and haptic channel isolation. For physical validation, connect each pad directly and check gameplay inputs, prompt changes, vibration, and reconnection. With DualSense, check trigger reset on release and focus loss, USB haptic output, and standard Bluetooth rumble.

The Steam executable audited for this release has SHA-256 `a4ba454b0e2a9190a42db631574b694cc0dbdcec750106c1b0364b70a6e4d17e`. The patch verifies the expected native instructions before redirecting game code at runtime.

The packaging command writes a ZIP and its SHA-256 file to `dist/`. Use the build that passed testing as the input. GitHub generates the source archives from the tagged commit.

## Windows setup

Use the .NET 10 SDK or newer on Windows to build the x64 WPF setup targeting .NET Framework 4.8. The private payload DLL embeds the tested x86 patch ZIP. Setup uses the Windows installation of .NET Framework.

```powershell
$patch = (Resolve-Path "dist/AC2-Enhanced-Patch-v$version-win32.zip").Path
$setup = Join-Path $env:TEMP ("ac2-setup-" + [guid]::NewGuid().ToString('N'))
dotnet build installer/Setup.csproj -c Release -p:DebugType=None -p:DebugSymbols=false `
    "-p:PatchZip=$patch" "-p:Version=$version" -o $setup
python tools/package_setup.py $version --exe "$setup/AC2-Enhanced-Patch-Setup.exe" --patch $patch
```

Packaging verifies that the private DLL contains the exact manual ZIP, including its DLLs and prompt artwork. Keep the setup executable, its configuration file, and the private DLL together. `--preview <file.png>` renders the setup window and `--prompt-preview <file.png>` exports the artwork shown by setup. `--verify-languages <folder>` verifies the 25 translation entries and renders the setup for each language. `--verify <isolated-game-fixture>` checks installation, repeat install, restore, and the prompt settings with a copy of the supported game executable. `--verify-drivers` downloads and checks each optional package.

## Visual and performance checks

Use `ac2_controller_smoke.exe --render-atlas <existing-folder>` to export the actual runtime sprites in display orientation, including both input states for every family. `tools/render_prompt_atlas.py` combines the tiles into a comparison sheet using Pillow. The build extracts the live PC keycap geometry and colors from SVG and rasterizes the four SVG controller sheets into embedded textures.

For frame-time comparisons, capture the same scene, viewpoint, resolution, and refresh and sync settings for each revision. Use `tools/compare_frame_traces.py` with the two CSV paths and matching steady frame ranges. It reports average FPS, median window p95 and p99, and worst frame time. For CPU and GPU memory comparisons, record matching per-process counters from the same sessions. The game executable is already large-address-aware.
