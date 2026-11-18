# Assassin’s Creed II Enhanced Patch

[Español](README.es.md) · [Download the latest release](../../releases/latest)

A community patch for the Windows Steam edition of Assassin’s Creed II.

## Features

- Full-screen gameplay at the selected aspect ratio, with proportional scaling for the HUD and menus.
- Edge-aligned action prompts, money, equipment and minimap. The radar stays filled and the weapon wheel and shop overlays reach the screen edges.
- Proportionate intro and pre-rendered video playback, with backgrounds extended to fill the screen.
- The internal 60 FPS wait is lifted when the selected refresh rate exceeds 60 Hz.
- Native input integration identifies supported DualShock 4 and Xbox controllers, selects the matching button glyphs, and forwards both vibration motors.

## Install

Requires the Windows Steam build **1843459** of app **33230**. In Steam, open the game's properties and browse its installed files.

1. Close the game. If `d3d9.dll` or `SDL3.dll` is already there, back up those copies outside the game folder.
2. From the release ZIP, place `d3d9.dll` and `SDL3.dll` beside `AssassinsCreedIIGame.exe`.
3. Launch from Steam. Choose your resolution and refresh rate in the game.

To uninstall, close the game, remove the two DLLs from this release, and restore any copies you backed up. Keep existing mods that use `d3d9.dll` in mind when deciding which DLL to load. The patch loads alongside the game and leaves the executable, saves, and settings intact.

## Compatibility and testing

Gameplay checks covered **1920×1200** and **1440×1080**. The geometry tests also cover 16:9, 3:2, 5:4, 4:5, 2:1 (18:9), 21:9, 2.375:1, 1.85:1, 2.35:1, 2.39:1, 2.40:1, 2.76:1, 9:16, 19.5:1 and 20:9.

Virtual-controller tests cover family switching, glyphs, reconnection, and sustained independent vibration. Physical USB and Bluetooth testing remains pending. For family-specific glyphs, Windows needs to expose the controller's hardware identity. Controller translation software can expose a different device.

The patch records local diagnostics in `%TEMP%\ac2-display-trace-*.csv`. See [build instructions](BUILD.md) for the compiler and test steps, and [third-party notices](THIRD_PARTY.md) for dependencies.

The patch source is under [MIT](LICENSE). Assassin’s Creed II is a Ubisoft game. This project is independent.
