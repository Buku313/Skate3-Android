# Skate 3 Mobile for Android

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

## Phone-only setup

The normal setup does not need a computer:

1. Install the APK and open **Skate 3 Mobile**.
2. Choose **Select My Skate 3 ISO**.
3. Use Android's file picker to select an ISO dumped from your own supported
   Xbox 360 copy. The ISO may be in Downloads, on an SD card, or on a connected
   USB drive.
4. Leave the app open while it inspects and extracts the ISO.
5. The app downloads the exact Title Update 3 package, verifies both patch
   files, and finishes the installation.
6. Choose **Play Skate 3**.

The app does not modify or upload the ISO. It validates the supported
USA/Europe `default.xex` and `data/webkit/EAWebkit.xex` with SHA-256. It also
validates the size and SHA-256 of both Title Update 3 payloads before making the
installation playable. A manual **Select Title Update File** fallback appears
if the download cannot be completed.

The extracted game uses app-specific external storage. Its usual location is:

```text
/storage/emulated/0/Android/data/chat.buku.skate3/files/game/
```

Android protects this folder from ordinary file-manager access. That is normal.
The installer and game can use it without All files access. Uninstalling the app
removes this folder, so keep the original ISO somewhere safe.

The extracted install is about 6.0 GiB. Keep about 8 GiB free for installation
headroom. If the ISO is stored on the same internal storage, the phone may need
about 15 GiB free in total until you delete or move the ISO.

Older development installs in `/sdcard/skate3` are still recognized when the
app already has All files access. New users do not need to create that folder or
grant that permission.

## Build prerequisites

- A macOS ARM build host
- CMake 3.25+, Ninja, and Homebrew LLVM/Clang
- Android SDK 35, Android NDK `27.2.12479018`, and JDK 17 or newer
- A legally obtained, extracted Skate 3 game dump in `game/`
- The Skate 3 Title Update 3 package at the repository root, or provided with
  `-DSKATE3_TITLE_UPDATE_PACKAGE=/path/to/package`

## Easy build

Non-coders can use the double-click
[macOS build guide](../docs/NONCODER_BUILD.md).

Put the extracted game in `game/` and the Title Update 3 package at the
repository root, then run this from the repository root:

```sh
./build-android.sh
```

The script detects Android Studio's SDK, NDK, and Java installation, initializes
submodules, generates the recompiled code, builds the native libraries, and
builds the APK. To use files stored elsewhere:

```sh
./build-android.sh \
  --game-dir /path/to/extracted-skate3 \
  --title-update /path/to/title-update-package
```

The finished APK is produced at `out/Skate3-Mobile-Android-debug.apk`. Use
`./build-android.sh --install` to build and install it on a connected device,
or `./build-android.sh --stage-game` to install it and copy your game data.

## Manual build

The wrapper above performs these steps automatically. To run them manually,
initialize the submodules first:

```sh
git submodule sync --recursive
git submodule update --init --recursive --jobs 8
```

Build the generated game code and Android native libraries, then build the APK:

```sh
export ANDROID_NDK_ROOT=/path/to/android-sdk/ndk/27.2.12479018
export SKATE3_TITLE_UPDATE_PACKAGE=/path/to/title-update-package
android/tools/build_android_libs.sh
(cd android && ./gradlew assembleDebug)
```

The manual build produces
`android/app/build/outputs/apk/debug/app-debug.apk`.

## Legacy developer staging

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

This staging route is retained for development and existing test devices. It
uses `/sdcard/skate3` and requires All files access. Normal users should use the
phone-only installer instead.

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

## Optional custom Turnip driver

On Snapdragon / Adreno devices, open **GPU Driver** in the launcher to import
an AdrenoTools-compatible driver ZIP. The launcher accepts the standard ADPKG
layout with `meta.json`, the named Vulkan library, and any dependency libraries.
It validates the metadata, Android API requirement, archive paths, extraction
limits, and ARM64 ELF files before activating the package.

The imported driver is stored under the app's private internal files directory
because Android will not load executable libraries from shared storage. It is
loaded without root through
[libadrenotools](https://github.com/bylaws/libadrenotools) before the first
Vulkan instance is created. The app does not bundle or automatically download
a Turnip package.

Driver packages are hardware-specific. Confirm the exact Adreno generation
before importing one, and only use a ZIP from a source you trust. Community
ADPKG builds are published by the
[AdrenoTools Drivers project](https://github.com/K11MCH1/AdrenoToolsDrivers/releases),
but neither that project nor any individual driver package is part of Skate 3
Mobile. A package that works on one Snapdragon model may crash or render
incorrectly on another.

**System Driver** is the default and permanent fallback. If a custom driver
causes a crash, reopen Skate 3 Mobile, choose **GPU Driver**, and select **Use
System Driver**. Mali devices such as the RG406V do not offer custom driver
activation.

## Optional SEIYU PARADISE PENGUIN MOD

Open **Mod Store** in the Skate 3 Mobile launcher and install Seiyu with one
tap. The store downloads and verifies these two assets:

```text
base.obj
texture_diffuse.png
```

The launcher copies them into the active installation. Press **RB + Start** in
game and open **Mods > Playable Character** to switch live between Seiyu and the
original skater. The renderer automatically rigs Seiyu to the live skater
skeleton while preserving the skateboard. A manual ZIP is also available on
the [Mod Store website](https://buku313.github.io/Skate3-Mobile/mods/).
