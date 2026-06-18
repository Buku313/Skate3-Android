<h1 align="center">🛹 Skate 3 — Android (aarch64) Port</h1>

<p align="center">
  <b>A native ARM64 Android port of Skate 3</b> — not an emulator.<br>
  The Xbox 360 game is statically recompiled to native machine code and runs directly on the device.<br>
  <b>Built for modern high‑end Android phones &amp; handhelds.</b>
</p>

<p align="center">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Android%2013%2B-3DDC84">
  <img alt="ABI" src="https://img.shields.io/badge/ABI-arm64--v8a-blue">
  <img alt="Renderer" src="https://img.shields.io/badge/renderer-Vulkan-red">
  <img alt="Best on" src="https://img.shields.io/badge/best%20on-Snapdragon%20%2F%20Adreno-orange">
</p>

---

## What this is

This is an **Android build** of [`skate3recomp`](https://github.com/mchughalex/skate3recomp) — a static
recompilation of the Xbox 360 version of Skate 3. The PowerPC game code is translated to C++ and compiled
to **native aarch64**, so the CPU side runs as real ARM machine code (no instruction emulation). Graphics
go through a **Vulkan** backend; windowing, input and audio through **SDL3**.

Because the CPU side is native, **the GPU is the only variable** — so it's built for **modern high‑end
Android devices**: recent Snapdragon/Adreno phones and newer Vulkan‑class handhelds, which expose the
GPU features the renderer needs and have the bandwidth for the Xbox 360 render‑target workload. It boots,
loads saves, takes controller input, and renders the game. On older/low‑end GPUs (e.g. Mali‑G57 class) it
still runs, but is GPU‑bound — see [Status](#status).

> ⚠️ **No game files are included.** You must provide your own legally‑dumped copy of Skate 3 (Xbox 360).

<p align="center"><img src="docs/img/demo.gif" width="70%"></p>

## Screenshots

<p align="center">
  <img src="docs/img/01-language-select.png" width="32%">
  <img src="docs/img/02-ea-blackbox-intro.png" width="32%">
  <img src="docs/img/03-difficulty-select.png" width="32%">
</p>
<p align="center"><i>Language select · intro playback (FFmpeg/VP6) · difficulty select — all rendering natively via Vulkan on Mali‑G57.</i></p>

## Status

| Area | State |
|---|---|
| Boot / front-end / menus | ✅ Works |
| Save data | ✅ Loads |
| Controller (built-in gamepad) | ✅ Works (SDL3 mapping) |
| Cutscene video (VP6/FFmpeg) | ✅ Plays |
| In-world rendering | ✅ Renders (GPU-dependent framerate) |
| Audio | ⚠️ Partial (XMA decoder issues) |
| Performance | High-end GPUs: good. Low-end tile GPUs (Mali-G57): GPU-bound (~10 fps), perf work ongoing |

Performance scales with the GPU. On capable Adreno/Snapdragon parts the renderer's required Vulkan
features (e.g. `vertexPipelineStoresAndAtomics`) are present and bandwidth is ample. On weaker tile GPUs
the Xbox 360's on-chip EDRAM — emulated by resolving render targets through main memory — becomes the
bottleneck; a tile-local render-target path (`VK_EXT_rasterization_order_attachment_access`) is the
headline optimization being pursued to bring those devices up.

## Device support

- **Recommended:** modern high-end Android phones / handhelds — recent **Snapdragon (Adreno)** or
  comparable Vulkan 1.3 GPUs, Android 13+.
- **Minimum / tested low end:** Anbernic RG406V (Unisoc T820, Mali-G57) — runs, GPU-bound.
- **ABI:** `arm64-v8a` only.

## Building

You need the **Android NDK r27**, **CMake + Ninja**, a host **Clang** (e.g. Homebrew `llvm`), **JDK 17**,
and your own extracted Skate 3 game dump. The build has two stages: a host-side code-generation pass
(recompiles the game to C++), then the cross-compile to `arm64-v8a`.

```sh
git clone --recurse-submodules https://github.com/Buku313/Skate3-Android.git
cd Skate3-Android

# 1. Extract default.xex + EAWebkit.xex from YOUR legally-dumped ISO into game/
python3 android/tools/extract_xiso.py /path/to/Skate3.iso game \
        default.xex data/webkit/EAWebkit.xex

# 2. Host codegen + cross-compile the native libs + stage them into the APK project
export ANDROID_NDK_ROOT=$HOME/Library/Android/sdk/ndk/27.2.12479018
android/tools/build_android_libs.sh

# 3. Build the APK
cd android
export JAVA_HOME=$(brew --prefix openjdk@17)/libexec/openjdk.jdk/Contents/Home
./gradlew assembleDebug
```

Output: `android/app/build/outputs/apk/debug/app-debug.apk`.

See [`android/README.md`](android/README.md) for the native-library layout and details.

## Installing + game data

```sh
adb install -r android/app/build/outputs/apk/debug/app-debug.apk

# Push the FULL extracted game data (~7 GB) to where the app looks for it:
android/tools/install_game_data.sh /path/to/Skate3.iso     # extracts + adb push to /sdcard/skate3/

# Grant "All files access" so it can read the dump:
#   Settings → Apps → Skate 3 → Permissions → All files access
```

## Performance tuning

Drop [`android/skate3-maxperf.toml`](android/skate3-maxperf.toml) into the app's config and toggle the
experimental cvars. `android/tools/perf_probe.sh on|dump` captures on-device GPU bucket timings,
render-pass counts, and resolve stats for profiling.

## How it works

```
Xbox 360 default.xex ──(host codegen)──▶ generated C++ ──(NDK clang)──▶ libskate3.so (native aarch64)
                                                                              │ links
                          librexruntime.so  (Xenia-derived runtime: PPC→C++ glue, kernel,
                          SDL3 + Vulkan + FFmpeg) ◀───────────────────────────┘
                                                                              │ loaded by
                                              SDLActivity (APK)  ──▶ SDL_main ──▶ game
```

## Credits

- **[skate3recomp](https://github.com/mchughalex/skate3recomp)** and the **rexglue SDK** — the
  recompilation this port is built on.
- **[Xenia](https://xenia.jp/)** — the GPU/kernel emulation lineage the runtime derives from.
- **[SDL3](https://libsdl.org/)**, **[FFmpeg](https://ffmpeg.org/)**, **Vulkan / volk / VMA**.

## Legal

This project contains **no game code or assets**. Skate 3 is © Electronic Arts. You must own and dump
your own copy. This is a non-commercial, fan-made compatibility/port effort distributed in the same
spirit as the upstream recompilation.
