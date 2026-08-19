# SKATE 3 MOBILE

<p align="center">
  <a href="https://buku313.github.io/Skate3-Mobile/"><strong>OPEN THE SKATE 3 MOBILE WEBSITE</strong></a>
  &nbsp;·&nbsp;
  <a href="https://github.com/Buku313/Skate3-Mobile/releases/latest/download/Skate3-Mobile-Android.apk"><strong>DOWNLOAD THE ANDROID APK</strong></a>
  &nbsp;·&nbsp;
  <a href="https://buku313.github.io/Skate3-Mobile/updates.html"><strong>UPDATES</strong></a>
  &nbsp;·&nbsp;
  <a href="https://github.com/Buku313/Skate3-Mobile/issues/new?template=bug_report.yml"><strong>REPORT A BUG</strong></a>
</p>

<p align="center">
  <img src="docs/skate3-android-cover.png" alt="Android mascot skateboarding in front of the Skate 3 logo" width="100%">
</p>

Skate 3 running as native ARM64 code on Android through static recompilation.
This is not an Xbox 360 emulator.

The current release is a developer build. The Android port is experimental,
but it is playable on the Anbernic RG406V.
It uses the native Vulkan renderer from Skate3Recomp with Android input, audio,
storage, performance profiles, and handheld tuning added in this fork.

No retail game files are included. You must provide your own legally obtained
copy of Skate 3. The phone installer obtains and verifies the supported Title
Update 3, with a manual file-selection fallback if its download is unavailable.

## Players: start here

You do not need a computer, developer tools, or an ISO extraction app.

1. [Download the latest signed Android APK](https://github.com/Buku313/Skate3-Mobile/releases/latest/download/Skate3-Mobile-Android.apk).
2. Install it and open **Skate 3 Mobile**.
3. Tap **Select My Skate 3 ISO** and choose an ISO dumped from your own copy.
4. Keep the app open while it extracts and verifies the game.
5. Tap **Play Skate 3**.

Requirements: Android 13 or newer, ARM64, Vulkan, and about 8 GiB free after
the ISO is already available. A multitouch controller is built in for phones.
Physical and built-in gamepads remain supported. The first verified device is
the Anbernic RG406V. This is experimental software, so other phones may need
testing and tuning.

Starting with v2.0.8, the launcher checks for signed updates automatically. It
downloads and verifies the new APK inside the app. Android still asks you to
approve the installation, but you do not need to return to GitHub or reinstall
your game files.

The original skater is selected by default. If you have the optional Seiyu
Paradise Penguin files, use the launcher to import `base.obj` and
`texture_diffuse.png`. In game, press **RB + Start**, open **Mods**, and switch
between **Original Skater** and **Seiyu Paradise Penguin** instantly.

## How it works and how it was developed

This project is a fork of Alex McHugh's
[Skate3Recomp](https://github.com/mchughalex/skate3recomp), not a new port made
from scratch. The upstream Git history and authorship are preserved.

The build uses these methods:

1. The user supplies an extracted retail `default.xex`, the EAWebKit XEX, and
   Title Update 3. A local build step prepares the updated executables and patch
   files.
2. The rexglue code generator statically translates the configured Xbox 360
   PowerPC guest functions into native code. Android Clang then compiles and
   links that generated code as ARM64 libraries with the rexglue compatibility
   runtime. The finished game logic runs as native AArch64 rather than through
   a conventional CPU emulator.
3. Runtime hooks observe the game's mesh submissions, textures, shader state,
   constants, and frame data. The custom native scene renderer reconstructs
   that work with Vulkan shaders instead of directly running the original Xbox
   360 graphics pipeline.
4. SDL3 supplies the Android window, physical controller, and audio integration.
   A native XInput bridge merges the optional multitouch overlay into player
   one, including in the recomp menu. The phone installer reads the user's ISO
   through Android's system file picker, extracts it locally, verifies the
   supported executables, installs the exact Title Update 3 payload, and stores
   the result in private app storage. Older `/sdcard/skate3` installations
   remain supported.
5. The launcher checks a small manifest hosted by this repository, compares the
   version code, verifies the downloaded APK with SHA-256, and hands it to
   Android's package installer. Android also enforces the existing app-signing
   certificate. Updates never replace the extracted game directory.
6. The handheld profile lowers scene resolution and draw distance, simplifies
   materials, removes grass and costly post effects, reduces selected static
   rendering work, and uses native occlusion and frame pacing. Physics, player
   input, the board, menus, and HUD remain full-rate. The quality profile keeps
   substantially more of the native renderer enabled for faster devices.

Reverse engineering in this project uses executable analysis, function and
shader disassembly, runtime logging, graphics captures, targeted hooks, and
repeated testing on real hardware. Development and documentation in this fork
have also been assisted by OpenAI Codex. Buku313 reviewed the changes and
performed the Android and RG406V build, launch, control, graphics, and gameplay
testing.

The repository does not distribute retail executables, assets, Title Update
data, or generated game code. Those inputs and generated outputs stay local to
the person building the project. This is experimental research software, and a
60 FPS setting is a target and guest cap, not a performance guarantee.

## Current status

- Boots into the game
- Menus and controller input work
- Optional multitouch controller for phones
- Verified in-app APK update checks
- In-app GitHub bug reports with privacy-safe device and crash diagnostics
- Gameplay and tricks work
- Native Vulkan rendering works
- 4 KB and 16 KB Android memory pages are supported
- Saves and game data load from Android storage
- RG406V performance profile included
- High-end Android quality profile included
- Optional Seiyu Paradise Penguin Mod included
- Experimental direct-IP free-skate ghost included and disabled by default

This is still early. Expect visual bugs, missing effects, device-specific
problems, and performance differences between phones.

## Device requirements

- Android 13 or newer
- ARM64
- ARMv8.2 with FP16 and dot-product support
- Vulkan support
- Touchscreen, physical controller, or built-in gamepad

The RG406V with the Unisoc T820 and Mali-G57 is the first verified device.
Newer Snapdragon and Adreno devices are good candidates, but they have not all
been tested.

## Graphics profiles

The same APK includes both profiles.

### RG406V / Performance

- 512x288 internal 3D scene
- Full-resolution menus and HUD
- Reduced world and LOD range
- Simplified materials
- Grass and expensive post effects disabled
- 60 FPS guest cap

### High-End / Quality

- 1280x720 internal 3D scene
- Original world and LOD range
- Vegetation and full material layers
- 2x MSAA
- Shadows, SSAO, bloom, and volumetric lighting
- 60 FPS guest cap

Open the recomp menu with **RB + Start**. Go to **Video**, select **Android
Device Profile**, then use **Apply & Restart**.

## Game data

### Phone-only setup

No laptop, command line, ISO extractor, or Android file manager is required:

1. Install the Skate 3 Mobile APK.
2. Open it and choose **Select My Skate 3 ISO**.
3. Pick an ISO dumped from your own supported Xbox 360 copy.
4. Keep the app open while it extracts about 6.0 GiB and downloads the verified
   1.7 MiB Title Update 3.
5. Press **Play Skate 3**.

The ISO is read directly from Downloads, an SD card, or a USB drive through the
Android file picker. It is never modified or uploaded. The extracted game stays
inside this app's storage. You can delete the ISO after setup if you do not need
it on the phone.

Allow about 8 GiB of free device storage for the installed game. If the ISO is
also copied to internal storage, temporary total use can be about 15 GiB until
you remove the ISO. Uninstalling the app removes its app-owned game files, so
keep your original dump.

The installer accepts the currently supported USA/Europe retail executables and
checks them with SHA-256 before launch. If the automatic title-update download
is unavailable, it offers a button to select your own Title Update 3 package.

Existing tester installs in `/sdcard/skate3` are detected and still work when
the app already has Android's All files access permission.

Full setup, storage, and developer build details are in
[android/README.md](android/README.md).

## Build

You need Android SDK 35, Android NDK r27c, JDK 17 or newer, CMake, Ninja,
Clang, your extracted game dump, and Title Update 3.

### Personal custom build without Terminal

On an Apple Silicon Mac:

1. Install and open [Android Studio](https://developer.android.com/studio) once.
2. Double-click `Setup Build Tools.command`.
3. Double-click `Build Skate 3 Mobile.command` and choose your game files.

The Mac builder is only needed to make a fresh personal APK or modify native
code. Normal players can use the phone-only setup above. The builder can create
the APK by itself or build, install, and copy the game to a connected Android
device. See the complete
[non-coder build guide](docs/NONCODER_BUILD.md).

### One-command build

Put the extracted game in `game/` and the Title Update 3 package at the
repository root, then run one command:

```sh
git clone --recursive https://github.com/Buku313/Skate3-Mobile.git
cd Skate3-Mobile
./build-android.sh
```

The script detects Android Studio's SDK, NDK, and Java installation, generates
the recompiled code, builds the native libraries, and builds the APK. Custom
game and title-update locations are also supported:

```sh
./build-android.sh \
  --game-dir /path/to/extracted-skate3 \
  --title-update /path/to/title-update-package
```

The finished APK is written to `out/Skate3-Mobile-Android-debug.apk`. Add
`--install` to install it on a connected Android device, or `--stage-game` to
install the APK and copy your game data. The complete manual build and device
setup are documented in [android/README.md](android/README.md).

## SEIYU PARADISE PENGUIN MOD

On Android, use the mod-files button in the launcher to import the user-supplied
Seiyu model and texture. The files are stored under the active game install:

```text
mods/penguin/base.obj
mods/penguin/texture_diffuse.png
```

Choose Seiyu from **RB + Start > Mods > Playable Character**. The mod rigs Seiyu
to the live skater skeleton and keeps the skateboard visible.
The model and texture are not included in this repository.

## Credits

### Alex McHugh, mchughalex

[Alex McHugh](https://github.com/mchughalex) created and maintains the upstream
[Skate3Recomp](https://github.com/mchughalex/skate3recomp) project and the
[Skate-specific rexglue runtime](https://github.com/mchughalex/rexglue-skate3).
His work is the foundation of this project, including the static recompilation
pipeline, native renderer, game integration, settings, tools, and a huge amount
of reverse engineering. The original commits and authorship are preserved in
this repository.

### Buku313 / Antonio Seevers

This fork contains the Android ARM64 port, SDL Android integration, RG406V
bring-up and optimization, Android graphics profiles, the Seiyu Paradise
Penguin Mod, and the experimental free-skate networking work.

### Projects used

- [rexglue SDK](https://github.com/rexglue/rexglue-sdk)
- [Xenia](https://github.com/xenia-project/xenia)
- [SDL](https://github.com/libsdl-org/SDL)
- [Vulkan](https://www.vulkan.org/)
- [FFmpeg](https://ffmpeg.org/)

Thank you to everyone whose Xbox 360 research, testing, bug reports, and open
source work made this possible.

## Legal

This is an unofficial fan project. It is not affiliated with Electronic Arts,
Black Box, Microsoft, Google, or the Android project.

Skate 3, its characters, names, and related assets belong to their respective
owners. Android is a trademark of Google LLC. This repository does not contain
the game, title update, DLC, generated game code, or other copyrighted retail
data.
