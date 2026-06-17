#!/usr/bin/env bash
# Cross-compile the Skate 3 native libraries for Android (arm64-v8a) and stage
# them into the Gradle project's jniLibs. Run from the repo root.
#
# Prereqs:
#   - Android NDK r27c at $ANDROID_NDK_ROOT (or edit NDK below)
#   - Homebrew LLVM (clang) for the HOST codegen step (brew install llvm)
#   - game/default.xex + game/data/webkit/EAWebkit.xex extracted from your ISO
#     (see tools/extract_xiso.py)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

NDK="${ANDROID_NDK_ROOT:-$HOME/Library/Android/sdk/ndk/27.2.12479018}"
BUILD=out/build/android-apk
JNI=android/app/src/main/jniLibs/arm64-v8a

# 1. Host codegen (only if not already generated).
if [ ! -f generated/sources.cmake ]; then
  echo ">> Running host codegen (needs Homebrew LLVM + the game dump in game/)..."
  cmake --preset macos-relwithdebinfo -DSKATE3_GAME_DATA_ROOT="$ROOT/game"
  cmake --build --preset macos-relwithdebinfo --target generate-all --parallel
  cmake --preset macos-relwithdebinfo -DSKATE3_GAME_DATA_ROOT="$ROOT/game"
fi

# 2. Cross-compile the game + runtime for arm64-v8a (perf-tuned, c++_shared).
echo ">> Configuring Android build..."
cmake -S . -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release -DSKATE3_GAME_DATA_ROOT="$ROOT/game" \
  -DCMAKE_C_FLAGS="-march=armv8.2-a+fp16+dotprod" \
  -DCMAKE_CXX_FLAGS="-march=armv8.2-a+fp16+dotprod" \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
  -DCMAKE_SHARED_LINKER_FLAGS_RELEASE="-Wl,--strip-debug" \
  -DREXGLUE_BUILD_TESTS=OFF -DREXGLUE_ENABLE_TRACY=OFF \
  -DREXGLUE_ENABLE_PERF_COUNTERS=OFF -DREXGLUE_USE_VULKAN=ON

echo ">> Building libskate3.so (+ librexruntime.so)..."
cmake --build "$BUILD" --target skate3 -j"$(sysctl -n hw.ncpu)"

# 3. Stage native libs into the APK project.
echo ">> Staging jniLibs..."
mkdir -p "$JNI"
cp "$BUILD/libskate3.so" "$JNI/"
cp "$BUILD/librexruntime.so" "$JNI/"
cp "$NDK/toolchains/llvm/prebuilt/"*/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so "$JNI/"

echo ">> Done. jniLibs:"
ls -lh "$JNI/"
echo ">> Build the APK:  cd android && ./gradlew assembleDebug"
