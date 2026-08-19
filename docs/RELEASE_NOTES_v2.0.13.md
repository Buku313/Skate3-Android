# Skate 3 Mobile v2.0.13 Compatibility Preview

This prerelease is for compatibility testing. v2.0.12 remains the recommended
public build while v2.0.13 is tested across more phones.

## What changed

- Added optional per-app custom Turnip driver support for Snapdragon and Adreno
  devices through libadrenotools.
- Added a GPU Driver menu that can import, select, replace, remove, and report an
  AdrenoTools ADPKG ZIP.
- Kept System Driver as the default and permanent recovery option.
- Validated imported metadata, archive paths, extraction sizes, Android API
  requirements, and ARM64 shared libraries before activation.
- Fixed Seiyu's skeleton palette selection so his feet, body, and flippers stay
  attached to the live skater animation while the board remains visible.
- Added the new Skate 3 app icon and launcher name.
- Added BUKU and 313 GitHub links to the launcher.
- Prevented touch input from duplicating native handheld controls.
- Added the selected GPU driver to bug reports.

## Turnip testing

Try System Driver first. If the game still crashes or renders a black screen on
a Snapdragon device, return to the launcher, open GPU Driver, and import an
AdrenoTools-compatible Turnip ZIP built for the device's Adreno generation.

Custom drivers are not bundled or downloaded by the app. They are experimental
and a package that works on one GPU may fail on another. If a custom driver
crashes, reopen the launcher and select System Driver.

When reporting results, include:

- Phone or handheld model
- Android version
- Adreno GPU or Snapdragon SoC
- System Driver or the exact custom driver package
- Whether the crash happens at Play, loading, menus, or gameplay
- The complete text from Bug Report / Device Diagnostics

## Verified before publishing

- Native ARM64 release build
- Android lint and APK assembly
- Release signing identity matches v2.0.12
- 4 KB and 16 KB Android native-library alignment
- RG406V System Driver launch, controls, gameplay, touch overlay, and Seiyu

The APK does not contain retail Skate 3 game data. Use a legally obtained game
dump and follow the setup instructions in the repository.
