# Skate 3 Mobile v2.0.17

This developer build focuses on the loading and startup failures reported on Samsung, AYN, and Retroid devices.

## What changed

- FMV fallback now stays on the emulated output until the movie decoder actually stops. This removes the repeated half-second native and emulated renderer loop seen in Retroid logs.
- Added narrow callback recoveries for the exact Samsung guest return address `0x82B328D4` and AYN guest return address `0x82F2D074` supplied in diagnostics.
- Added exact first-run presentation recoveries at `0x82805478`, `0x828DF620`, and `0x82958D0C`. Each site was checked against its recompiled caller so unused notifications are skipped and the service query follows its existing null fallback.
- Rebuilt the optional GPU driver chooser with visible, focusable buttons. This avoids vendor dialog layouts that hid the old list on Samsung, Retroid, and AYN devices.
- System Driver remains selected and recommended by default. Turnip imports are optional troubleshooting tools and never activate automatically.

## RG406V regression checks

The release candidate completed five initial cold launches on the RG406V with the System Driver. Testing covered full intro playback, title screens, two gameplay entries, pause and resume, and physical controller input. A separate empty-profile stress set repeatedly exercised language selection and an immediate FMV skip. The final candidate logs contained no fatal call, SIGABRT, or SIGSEGV entry.

## Still needs affected-device testing

The callback fixes are based on exact diagnostics from affected devices. They do not reproduce on the RG406V, so fresh confirmation is still needed from Samsung, AYN, and Retroid owners. Please use the Bug Report button if v2.0.17 still stops during loading and include the new diagnostics.
