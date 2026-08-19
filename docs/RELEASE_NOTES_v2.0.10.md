# Skate 3 Mobile v2.0.10

Snapdragon compatibility test build for the Android developer port.

## Changes

- Performance mode no longer compiles disabled shadow, outline-edge, or spline Vulkan pipelines during the gameplay loading transition.
- The eager triangle-list and triangle-strip pipeline lifetime remains intact after RG406V A/B testing found that deferring strip creation could stall the boot sequence.
- The Bug Report button now reads the real saved graphics profile instead of reporting "I do not know" for the default install path.
- Native crashes now include the signal, abort message, crashing thread, and up to 16 stack-frame library offsets from Android's tombstone data.
- Reports include a filtered, privacy-safe excerpt of the latest Vulkan and native-renderer log.
- The browser report is length-limited while the complete report is copied to the clipboard.

## Verification

- Signed ARM64 APK build completed.
- Java compile and Android lint completed.
- Clean launch, language selection, first-time setup, and live native gameplay verified on an Anbernic RG406V.
- Gameplay reached 60 FPS with the native renderer active on the RG406V.
- The APK contains no retail Skate 3 game data.

This remains an experimental developer build. Snapdragon compatibility still needs confirmation from affected testers.
