# Skate 3 Mobile v2.0.12

One-tap Mod Store and Seiyu Paradise Penguin release.

## Changes

- Adds a Mod Store button to the phone-only launcher after game setup.
- Publishes Seiyu Paradise Penguin as the first downloadable character mod.
- Installs, reinstalls, and removes mods without a laptop or external file manager.
- Keeps the original skater available through **RB + Start > Mods > Playable Character**.
- Retains manual OBJ and PNG import for older builds and offline use.
- Publishes a responsive Mod Store page and a 2.18 MiB manual ZIP on the project website.
- Includes the Android guest-memory and Qualcomm compatibility work from v2.0.11.

## Download safety

- The launcher accepts only HTTPS mod catalogs and assets from approved project hosts.
- Every asset has a strict maximum size, exact expected byte count, and SHA-256 checksum.
- Install paths are restricted to the active game's `mods` directory.
- Files download to temporary paths and replace installed assets only after verification.

## Verification

- The signed ARM64 APK built successfully and passed Android release lint.
- APK signature, 16 KB ZIP alignment, version metadata, and absence of retail game data were checked.
- The live GitHub catalog and both Seiyu assets returned successfully.
- The complete catalog, download, SHA-256 verification, install, Manage, Reinstall, and Remove UI flow was tested on an Anbernic RG406V.
- In-place installation preserved the public game files and settings.
- The release candidate opened the native Skate 3 activity on the RG406V without a Java or native crash.

This remains an experimental developer build. Device-specific graphics and startup issues can still occur, especially on unverified Snapdragon and Exynos hardware.
