# Skate 3 Mobile v2.0.11

Android guest-memory and Qualcomm compatibility test build.

## Changes

- Android now uses a sparse `memfd` backing object for the 4.5 GB Xbox 360 guest address space, with the platform shared-memory API retained as a fallback.
- The complete guest address range is reserved before fixed alias views are installed, preventing the game from replacing unrelated Android runtime mappings.
- Android can fall back to an arbitrary safe 64-bit base when the preferred guest-memory addresses are unavailable.
- Qualcomm Android devices use the existing fully bound shared-memory buffer instead of Vulkan sparse residency while Adreno compatibility is being validated.
- Runtime logs now record the successful guest-memory base and the Qualcomm shared-memory decision.
- Bug reports include guest-address-space, shared-memory, and sparse-residency evidence.
- The issue form correctly renders the default Performance profile as one option.

## Verification

- The signed ARM64 APK built successfully and passed Android release lint.
- APK signature, 16 KB zip alignment, version metadata, and absence of retail game data were checked.
- In-place installation preserved the existing game files and settings on an Anbernic RG406V.
- Cold native startup, loading, touch input, pause-menu navigation, live gameplay, and the bug-report dialog were verified on the RG406V.
- Native gameplay reached 60 FPS on the RG406V without a fatal signal, Vulkan device loss, or guest-memory reservation error.

This is a prerelease developer build. The RG406V test protects the known-good Mali path, but affected Adreno, Exynos, and newer Android devices still need to confirm the compatibility changes on real hardware.
