# Build and test

Use Windows with Python 3, the [LLVM-MinGW UCRT toolchain](https://github.com/mstorsjo/llvm-mingw) on `PATH`, and the i686 folder from the [SDL 3.4.16 MinGW development package](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.16). The initial release used LLVM-MinGW 20261020. The video shader build uses Windows' `d3dcompiler_47.dll`.

Build the controller artwork with [resvg](https://github.com/linebender/resvg/releases) and Python's Pillow. Put `resvg.exe` on `PATH` or pass `--resvg` with the executable path. Both are build tools.

```powershell
$sdl = 'C:\SDKs\SDL3-3.4.16\i686-w64-mingw32'
python tools/build_d3d9_probe.py --native-view --fit-video --high-refresh --lean-trace --controller-sdk $sdl --experimental-audio

$build = 'build/native-view-video-fit-high-refresh-lean-trace-controllers-experimental-audio'
foreach ($name in 'display', 'view', 'video', 'controller') {
    & "$build/ac2_${name}_smoke.exe"
    if ($LASTEXITCODE -ne 0) { throw "Failed test $name" }
}

$version = '1.2.0'
$tiles = New-Item -ItemType Directory -Path build/prompt-atlas -Force
& "$build/ac2_controller_smoke.exe" --render-atlas $tiles.FullName
if ($LASTEXITCODE -ne 0) { throw 'Failed to export prompt artwork.' }
python tools/render_prompt_atlas.py $tiles.FullName "dist/input-prompts-v$version.png" --version $version
python tools/package_release.py $version --build-dir $build --prompt-preview "dist/input-prompts-v$version.png"
```

Close any exclusive full-screen application if the Direct3D tests cannot create their test device. Tests exercise Direct3D forwarding, aspect-ratio geometry, video fitting, remapped keyboard and mouse prompts, controller glyphs, virtual input and rumble, DualSense trigger reports, and haptic channel isolation. For physical validation, connect each pad directly and check gameplay inputs, prompt changes, vibration, and reconnection. With DualSense, check trigger reset on release and focus loss, USB haptic output, and standard Bluetooth rumble.

The Steam executable audited for this release has SHA-256 `a4ba454b0e2a9190a42db631574b694cc0dbdcec750106c1b0364b70a6e4d17e`. The patch verifies the expected native instructions before redirecting game code at runtime.

The packaging command writes a ZIP and its SHA-256 file to `dist/`. Use the build that passed testing as the input. GitHub generates the source archives from the tagged commit.

## Gameplay audio output

The release build includes the audio bridge through the `--experimental-audio` build flag. The bridge targets the supported Steam executable and its 48 kHz Wwise gameplay stream. It queries the game's default render endpoint with WASAPI and plays the Wwise 5.1 mix through XAudio2 2.9. DirectSound provides the compatibility fallback and handles Bink movie audio.

The CSV in the Windows temporary directory records the endpoint layout, incoming peaks per Wwise channel, and playback cursor. `audio_xaudio_fallback` records an interrupted bridge session.

See [Discrete audio outputs and channel verification](docs/audio-outputs.md) for output maps, the single-output tone diagnostic and a physical device test procedure.

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

Use `ac2_controller_smoke.exe --render-atlas <existing-folder>` to export the actual runtime sprites in display orientation, including both input states for every family and all 24 keyboard and mouse rows in Controls. `tools/render_prompt_atlas.py` combines the common tiles into a comparison sheet using Pillow.

Use `ac2_controller_smoke.exe --render-keyboard <existing-folder>` to export both states for the complete keyboard and mouse set. Run `python tools/render_keyboard_layout.py <tiles-folder> <output.png>` to arrange those tiles in a keyboard layout. `ac2_controller_smoke.exe --check-live-bindings` checks a binding change before it reaches the saved profile.

The build embeds `assets/prompts/keyboard-mouse.png` and rasterizes the four SVG controller sheets into embedded textures. To update the keyboard artwork, regenerate the PNG with `resvg assets/prompts/keyboard-mouse.svg assets/prompts/keyboard-mouse.png`. SDL decodes the PNG at runtime. The keyboard prompt renderer composites the mask or the live binding legend and filters the result into smaller texture levels.

For frame-time comparisons, capture the same scene, viewpoint, resolution, and refresh and sync settings for each revision. Use `tools/compare_frame_traces.py` with the two CSV paths and matching steady frame ranges. It reports average FPS, median window p95 and p99, and worst frame time. For CPU and GPU memory comparisons, record matching per-process counters from the same sessions. The game executable is already large-address-aware.
