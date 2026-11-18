# Assassin’s Creed II Enhanced Patch

A community patch for the Windows Steam edition of Assassin’s Creed II. It adapts gameplay and the interface to your display, supports refresh rates above 60 Hz, and adds native integration for supported DualShock 4 and Xbox controllers.

[Download the latest release](https://github.com/XssentialPilar/assassins-creed-ii-enhanced-patch/releases/latest) · [Installation](#installation) · [Compatibility](#compatibility)

![Ezio at a viewpoint over Monteriggioni, with the complete HUD visible at 1920 × 1200](docs/screenshots/monteriggioni-viewpoint-16x10.png)

*Monteriggioni at 1920 × 1200 (16:10). The action prompts and minimap remain visible at the right edge.*

## What the patch changes

- **Display and interface.** The game fills the selected resolution. HUD elements retain their proportions, the minimap fills its frame, and the shop and weapon-wheel overlays reach the screen edges.
- **Pre-rendered video.** Videos retain their original proportions while the surrounding background fills the display.
- **High refresh rates.** Selecting a refresh rate above 60 Hz removes the game’s internal 60 FPS wait.
- **Controller support.** Supported DualShock 4 and Xbox controllers use family-specific button icons. The patch forwards the game’s vibration commands to both motors.

| Action prompts and minimap at 1920 × 1200 | Weapon wheel overlay at 1440 × 1080 |
| --- | --- |
| ![Climbing HUD with the action prompts and minimap fully visible](docs/screenshots/rooftop-hud-16x10.png) | ![Weapon wheel centered over a translucent overlay that reaches all four edges](docs/screenshots/weapon-wheel-4x3.png) |

## Installation

The patch supports Steam app **33230**, Windows build **1843459**.

1. Close the game and download the Windows ZIP from the [latest release](https://github.com/XssentialPilar/assassins-creed-ii-enhanced-patch/releases/latest).
2. In Steam, open the game’s properties and browse its installed files. If the folder already contains `d3d9.dll` or `SDL3.dll`, please back up those copies before proceeding.
3. Extract both DLLs beside `AssassinsCreedIIGame.exe`. Launch the game through Steam and choose your resolution and refresh rate.

To uninstall, close the game, remove the DLLs you installed from the ZIP, and restore any copies you backed up. The patch leaves the game executable, saves, and settings intact.

## Compatibility

Gameplay was checked at **1920 × 1200 (16:10)** and **1440 × 1080 (4:3)**. Automated geometry tests also cover **16:9**, **3:2**, **5:4**, **4:5**, **2:1 (18:9)**, **21:9**, **2.375:1**, **1.85:1**, **2.35:1**, **2.39:1**, **2.40:1**, **2.76:1**, **9:16**, **19.5:1**, and **20:9**.

Virtual-controller tests cover family switching, distinct button icons, reconnection, and sustained vibration. Physical USB and Bluetooth validation is pending. Family-specific icons depend on the hardware identity exposed to Windows. Controller translation software may present the pad as a different device.

If you encounter a problem, please include the game build, resolution, and refresh rate when opening an issue. For controller reports, please also include the model and connection type. Local diagnostics are available in `%TEMP%\ac2-display-trace-*.csv`. [Report an issue](https://github.com/XssentialPilar/assassins-creed-ii-enhanced-patch/issues)

[Build and test](BUILD.md) · [MIT license](LICENSE) · [Third-party notices](THIRD_PARTY.md)

Assassin’s Creed II is a Ubisoft game. This community project is independent.
