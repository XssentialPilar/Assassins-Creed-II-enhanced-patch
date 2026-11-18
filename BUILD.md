# Build and test

Use Windows with Python 3, the [LLVM-MinGW UCRT toolchain](https://github.com/mstorsjo/llvm-mingw) on `PATH`, and the i686 folder from the [SDL 3.4.16 MinGW development package](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.16). The initial release used LLVM-MinGW 20261020. The video shader build uses Windows' `d3dcompiler_47.dll`.

```powershell
$sdl = 'C:\SDKs\SDL3-3.4.16\i686-w64-mingw32'
python tools/build_d3d9_probe.py --native-view --fit-video --high-refresh --lean-trace --controller-sdk $sdl

$build = 'build/native-view-video-fit-high-refresh-lean-trace-controllers'
foreach ($name in 'display', 'view', 'video', 'controller') {
    & "$build/ac2_${name}_smoke.exe"
    if ($LASTEXITCODE -ne 0) { throw "Failed test $name" }
}

python tools/package_release.py 1.0.0 --build-dir $build
```

Close any exclusive full-screen application if the Direct3D tests cannot create their test device. Tests exercise Direct3D forwarding, aspect-ratio geometry, the video render path, and virtual controller input and rumble.

The Steam executable audited for this release has SHA-256 `a4ba454b0e2a9190a42db631574b694cc0dbdcec750106c1b0364b70a6e4d17e`. The patch verifies the expected native instructions before redirecting game code at runtime.

The packaging command writes a ZIP and its SHA-256 file to `dist/`. Use the build that passed testing as the input. GitHub generates the source archives from the tagged commit.
