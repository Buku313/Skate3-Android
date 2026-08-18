# Skate 3 Native for Android

This is the experimental ARM64 Android target for Skate3Recomp. The Xbox 360
PowerPC guest code is statically recompiled to native AArch64 and linked with
the rexglue runtime, SDL3, and Vulkan. No retail game or title-update files are
included in this repository.

## Device requirements

- Android 13 or newer (API 33+)
- `arm64-v8a`
- ARMv8.2 CPU with FP16 and dot-product extensions
- Vulkan support
- A physical controller or built-in Android gamepad

The Anbernic RG406V (Unisoc T820 / Mali-G57) is verified. Other recent gaming
handhelds and sufficiently fast phones may work, but the build is not currently
universal: 32-bit devices, older ARM64 CPUs, Android 12 and older, and devices
without the required Vulkan features are unsupported.

This branch also contains the Seiyu Paradise Penguin Mod character replacement
and a disabled-by-default milestone-one direct-IP UDP free-skate ghost. The
networking prototype exchanges player poses and is not full Skate 3 online
multiplayer.

## Build prerequisites

- A macOS ARM build host
- CMake 3.25+, Ninja, and Homebrew LLVM/Clang
- Android SDK 35, Android NDK `27.2.12479018`, and JDK 17
- A legally obtained, extracted Skate 3 game dump in `game/`
- The Skate 3 Title Update 3 package at the repository root, or provided with
  `-DSKATE3_TITLE_UPDATE_PACKAGE=/path/to/package`

Initialize all submodules first:

```sh
git submodule sync --recursive
git submodule update --init --recursive --jobs 8
```

Build the generated game code and Android native libraries:

```sh
export ANDROID_NDK_ROOT=/path/to/android-sdk/ndk/27.2.12479018
android/tools/build_android_libs.sh
```

Then build the APK with JDK 17 active:

```sh
cd android
./gradlew assembleDebug
```

The APK is produced at `android/app/build/outputs/apk/debug/app-debug.apk`.

## Install and stage your game data

Install the package, then copy the contents of your own fully extracted game to
the app's shared-storage directory:

```sh
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell mkdir -p /sdcard/skate3
adb push /path/to/extracted-skate3/. /sdcard/skate3/
```

If the build used Title Update 3, also stage the two generated patch files:

```sh
adb push out/build/android-release/game/default.xexp /sdcard/skate3/default.xexp
adb shell mkdir -p /sdcard/skate3/data/webkit
adb push out/build/android-release/game/data/webkit/EAWebkit.xexp /sdcard/skate3/data/webkit/EAWebkit.xexp
```

On first launch, Android asks for All files access so the game can read
`/sdcard/skate3`. Grant it and return to the app.

## Handheld profile and controls

The same APK includes two device profiles:

| Profile | Scene | Intended hardware | Rendering |
| --- | --- | --- | --- |
| RG406V / Performance | 512x288 | T820/Mali-G57-class handhelds | 0.5x world/LOD range, simplified materials, no grass or expensive post effects |
| High-End / Quality | 1280x720 | Fast modern phones and gaming handhelds | Original world/LOD range, vegetation and full materials, 2x MSAA, shadows, SSAO, bloom and volumetrics |

Open the recomp overlay with **RB + Start**, then choose **Video > Android Device
Profile** and use **Apply & Restart**. Performance remains the default so an APK
can boot safely on the RG406V. Both profiles keep the 60 FPS guest cap; the map,
player physics, and board remain full-rate. The controller is presented to the
game as an Xbox pad through SDL.

High-End is deliberately demanding and is not guaranteed to sustain 60 FPS on
every nominally compatible phone. If it overheats, crashes a GPU driver, or falls
below full speed, return to the Performance profile.

## Optional SEIYU PARADISE PENGUIN MOD

Place the two user-supplied mod assets here:

```text
/sdcard/skate3/mods/penguin/base.obj
/sdcard/skate3/mods/penguin/texture_diffuse.png
```

The renderer automatically rigs Seiyu to the live skater skeleton while
preserving the skateboard. The assets themselves are intentionally not part of
the repository.
