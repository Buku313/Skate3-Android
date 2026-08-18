# Skate 3 Mobile v2.0.7

This hotfix adds reliable support for modern Android devices that use 16 KB
memory pages, including current flagship phones.

## Fixed

- Rebuilt every bundled ARM64 shared library with 16 KB ELF alignment.
- Made native guest-memory protection follow the device's actual page size.
- Preserved compatibility with existing 4 KB Android handhelds.

The APK does not include retail Skate 3 game data. Import an ISO dumped from
your own copy using the phone-only installer.
