#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
game_root="${SKATE3_GAME_DATA_ROOT:-${project_root}/game}"
android_ndk="${ANDROID_NDK_ROOT:-}"
title_update="${SKATE3_TITLE_UPDATE_PACKAGE:-}"
jni_dir="${project_root}/android/app/libs/arm64-v8a"

if [[ -z "${android_ndk}" ]]; then
  echo "ANDROID_NDK_ROOT must point to Android NDK 27.2.12479018" >&2
  exit 1
fi

if [[ ! -f "${game_root}/default.xex" ||
      ! -f "${game_root}/data/webkit/EAWebkit.xex" ]]; then
  echo "Missing extracted Skate 3 data under ${game_root}" >&2
  exit 1
fi

cd "${project_root}"

cmake_args=(-DSKATE3_GAME_DATA_ROOT="${game_root}")
if [[ -n "${title_update}" ]]; then
  cmake_args+=(-DSKATE3_TITLE_UPDATE_PACKAGE="${title_update}")
fi

if [[ ! -f generated/sources.cmake ||
      ! -f generated/eawebkit/sources.cmake ]]; then
  cmake --preset macos-relwithdebinfo "${cmake_args[@]}"
  cmake --build --preset macos-relwithdebinfo --target generate-all --parallel
fi

cmake --preset android-release "${cmake_args[@]}"
cmake --build --preset android-release --parallel

cxx_shared="$(find "${android_ndk}/toolchains/llvm/prebuilt" \
  -path '*/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so' \
  -print -quit)"
if [[ -z "${cxx_shared}" ]]; then
  echo "Could not locate libc++_shared.so under ${android_ndk}" >&2
  exit 1
fi

mkdir -p "${jni_dir}"
install -m 0755 out/build/android-release/libskate3.so "${jni_dir}/"
install -m 0755 out/build/android-release/librexruntime.so "${jni_dir}/"
install -m 0755 "${cxx_shared}" "${jni_dir}/"

echo "Android libraries staged in ${jni_dir}"
