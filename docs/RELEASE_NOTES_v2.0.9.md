# Skate 3 Mobile v2.0.9

This developer update adds useful in-app crash reporting for device testing.

## Added

- A Bug Report / Device Diagnostics button in the Android launcher.
- Automatic GitHub issue-form prefilling for the app version, device, Android
  version, SoC, graphics profile, and input method.
- Memory-page size and reported Vulkan feature version diagnostics.
- Android historical process-exit reasons, including native crashes, Java
  crashes, ANRs, low-memory kills, signals, and initialization failures.
- A clipboard copy of the same diagnostics in case a browser removes a field.

The report deliberately excludes ISOs, game executables, saves, account names,
and private filesystem paths. GitHub sign-in and final issue submission still
require user confirmation.

This remains an experimental developer build. The new diagnostics do not claim
to fix every Snapdragon crash yet. They are designed to capture enough exact
device and exit information to reproduce and fix those failures.
