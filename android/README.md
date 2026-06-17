# Skate 3 — Android (aarch64) APK

Native Android port of the Skate 3 recompilation for the RG406V (Unisoc T820, Mali-G57,
Android 13 / arm64-v8a). No x86 translation — the game is recompiled to native ARM.

## Native library layout
The game ships as three native libs in `app/src/main/jniLibs/arm64-v8a/`:

| Lib | What |
|-----|------|
| `libc++_shared.so` | Shared C++ runtime (one copy across all libs) |
| `librexruntime.so` | rexglue Xbox 360 recomp runtime; statically links SDL3 and hosts the SDL Android JNI bridge (`Java_org_libsdl_app_*`, `JNI_OnLoad`) |
| `libskate3.so` | The recompiled game; exports `SDL_main` (entry point) |

`Skate3Activity` (extends SDL's `SDLActivity`) loads them in order:
`c++_shared` → `rexruntime` → `skate3`, and SDL invokes `SDL_main` in `libskate3.so`.

## Build

1. **Cross-compile the native libs** (needs Android NDK r27c + Homebrew LLVM for host
   codegen + your extracted game dump in `../game/`):
   ```sh
   ../tools/build_android_libs.sh
   ```
   This runs host codegen (once), cross-compiles `libskate3.so` + `librexruntime.so`
   (perf-tuned: `-O3` + thin-LTO + `armv8.2-a`, stripped), and stages `jniLibs/`.

2. **Build the APK:**
   ```sh
   export JAVA_HOME=$(brew --prefix openjdk@17)/libexec/openjdk.jdk/Contents/Home
   ./gradlew assembleDebug
   ```
   Output: `app/build/outputs/apk/debug/app-debug.apk`.

## Install + game data

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
# Push the FULL extracted ISO (~7 GB) to where the game looks for it:
../tools/install_game_data.sh /path/to/Skate3.iso
```

The player must supply their own legally-dumped Skate 3 ISO. Game data lives at
`/sdcard/skate3/` (`default.xex` + `data/...`). Grant **All files access** to the app
(Settings → Apps → Skate 3 → Permissions) so it can read external storage.

## Controller
SDL3's Android input layer (`SDLControllerManager`) maps the RG406V's built-in pad to a
standard Xbox layout, which is what Skate 3 expects. Verify on-device with
`adb logcat | grep -i 'controller\|gamepad\|joystick'` after launch. If the pad reports a
non-standard mapping, add an `SDL_GameControllerAddMapping` entry (GUID from the logcat line).

## Notes
- `extractNativeLibs=true` / `useLegacyPackaging=true`: the libs are large (game ~80 MB);
  they're extracted at install so the dynamic linker can load them.
- Renderer brings up via `VK_KHR_android_surface` (volk-loaded Vulkan). Vulkan surfaces
  don't appear in CDP screenshots — use `adb logcat` + on-device eyes for renderer debugging.
