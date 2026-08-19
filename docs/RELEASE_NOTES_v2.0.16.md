# Skate 3 Mobile v2.0.16

Compatibility and handheld display release based on the newest community crash reports.

## Qualcomm and Turnip

- Compacts the native scene renderer from seven Vulkan descriptor sets to four,
  the Vulkan portability floor. This directly targets the system-driver crash
  reported on Adreno 619 and 650 during pipeline-layout creation.
- Keeps System Driver selected by default. Custom Turnip packages remain an
  optional test path under Advanced Options and are never enabled automatically.
- Adds a direct Use System Driver recovery action whenever a custom driver is
  active.
- Accepts both ZIP and ADPKG driver packages in Android's file picker.
- Fixes unreadable white-on-white GPU driver dialogs seen on Samsung and Retroid
  devices.

## Native renderer and 4:3 handhelds

- Adds a native 4:3 display mode for devices such as the RG406V.
- Keeps the gameplay HUD inside a 16:9 safe area while the 3D scene fills the
  4:3 display, preventing the combo meter and minimap from being cropped.
- Recovers the exact stale cleanup callback reported by AYN Thor diagnostics at
  guest return address `0x82B3C95C`.

## Verification

- Native ARM64 release build and Android APK assembly
- Cold System Driver launch through language selection and title screen
- Live native gameplay on RG406V at 60 FPS
- Native textures, controller input, 4:3 HUD placement, and included Seiyu mod
- Second cold relaunch on RG406V with no native crash

The Qualcomm fixes are based on supplied crash stacks and still need confirmation
on the affected Adreno 619 and 650 devices. Please include fresh v2.0.16
diagnostics if a crash remains.

The APK does not contain retail Skate 3 game data. Use a legally obtained game
dump and follow the setup instructions in the repository.
