# Skate 3 Mobile v2.0.14

Seiyu is now part of Skate 3 Mobile while the original skater remains the
default.

## What changed

- Bundled the verified Seiyu Paradise Penguin model and texture in the APK.
- Added automatic local installation and exact SHA-256 verification for the
  included character files.
- Kept Original Skater selected by default.
- Kept the live **RB + Start > Mods > Playable Character** switch between the
  original skater and Seiyu.
- Preserved the verified Mod Store for community character releases.
- Added a public character-submission page and structured GitHub review form.
- Moved custom GPU driver controls to **Advanced Options > GPU driver
  experiments**.
- Kept System Driver selected after importing a custom driver. Enabling one now
  requires a separate warning and confirmation.

## Character submissions

Creators can visit the
[submission page](https://buku313.github.io/Skate3-Mobile/mods/submit.html) for
the package format, testing checklist, and GitHub submission form. Submitted
files must be original or authorized for redistribution and may not contain
retail Skate 3 assets.

## Verified before publishing

- Android lint and debug APK assembly
- Embedded Seiyu assets match the approved files by SHA-256
- Automatic Seiyu restore into the active game installation
- Original skater setting disabled during the clean RG406V launch test
- System Driver launch to the Skate 3 title screen at 60 FPS on RG406V
- Release APK signing and 4 KB plus 16 KB native-library alignment

The APK does not contain retail Skate 3 game data. Use a legally obtained game
dump and follow the setup instructions in the repository.
