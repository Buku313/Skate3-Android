#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
game_root="${SKATE3_GAME_DATA_ROOT:-${project_root}/game}"
title_update="${SKATE3_TITLE_UPDATE_PACKAGE:-}"
android_ndk="${ANDROID_NDK_ROOT:-}"
install_apk=false
stage_game=false
ndk_version="27.2.12479018"

usage() {
  cat <<'EOF'
Build the Skate 3 Android APK from a legally obtained game dump.

Usage:
  ./build-android.sh [options]

Options:
  --game-dir PATH       Extracted Skate 3 directory (default: ./game)
  --title-update PATH   Skate 3 Title Update 3 package
  --ndk PATH            Android NDK 27.2.12479018 directory
  --install             Install the finished APK with adb
  --stage-game          Install the APK and copy your game data to the device
  -h, --help            Show this help

Environment alternatives:
  SKATE3_GAME_DATA_ROOT
  SKATE3_TITLE_UPDATE_PACKAGE
  ANDROID_NDK_ROOT
  ANDROID_SDK_ROOT or ANDROID_HOME
  JAVA_HOME
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

step() {
  printf '\n==> %s\n' "$*"
}

need_command() {
  command -v "$1" >/dev/null 2>&1 || die "$2"
}

absolute_dir() {
  [[ -d "$1" ]] || die "directory not found: $1"
  (cd "$1" && pwd -P)
}

absolute_file() {
  [[ -f "$1" ]] || die "file not found: $1"
  local parent name
  parent="$(dirname "$1")"
  name="$(basename "$1")"
  printf '%s/%s\n' "$(cd "$parent" && pwd -P)" "$name"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --game-dir)
      [[ $# -ge 2 ]] || die "--game-dir requires a path"
      game_root="$2"
      shift 2
      ;;
    --title-update)
      [[ $# -ge 2 ]] || die "--title-update requires a path"
      title_update="$2"
      shift 2
      ;;
    --ndk)
      [[ $# -ge 2 ]] || die "--ndk requires a path"
      android_ndk="$2"
      shift 2
      ;;
    --install)
      install_apk=true
      shift
      ;;
    --stage-game)
      install_apk=true
      stage_game=true
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1 (run ./build-android.sh --help)"
      ;;
  esac
done

[[ "$(uname -s)" == "Darwin" && "$(uname -m)" == "arm64" ]] ||
  die "the current Android build requires an Apple Silicon Mac"

need_command git "Git is required"
need_command cmake "CMake 3.25 or newer is required (brew install cmake)"
need_command ninja "Ninja is required (brew install ninja)"

[[ -x /opt/homebrew/opt/llvm/bin/clang ]] ||
  die "Homebrew LLVM is required (brew install llvm)"

game_root="$(absolute_dir "$game_root")"
[[ -f "${game_root}/default.xex" ]] ||
  die "missing ${game_root}/default.xex"
[[ -f "${game_root}/data/webkit/EAWebkit.xex" ]] ||
  die "missing ${game_root}/data/webkit/EAWebkit.xex"

if [[ -n "$title_update" ]]; then
  title_update="$(absolute_file "$title_update")"
elif [[ -f "${project_root}/TU_12K2276_000000C000000.00000000000O3" ]]; then
  title_update="${project_root}/TU_12K2276_000000C000000.00000000000O3"
else
  die "Title Update 3 is required. Put TU_12K2276_000000C000000.00000000000O3 in the repository root or pass --title-update PATH"
fi

android_sdk="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
if [[ -z "$android_sdk" && -d "${HOME}/Library/Android/sdk" ]]; then
  android_sdk="${HOME}/Library/Android/sdk"
fi
[[ -n "$android_sdk" ]] ||
  die "Android SDK not found. Set ANDROID_SDK_ROOT or install Android Studio"
android_sdk="$(absolute_dir "$android_sdk")"

if [[ -z "$android_ndk" && -d "${android_sdk}/ndk/${ndk_version}" ]]; then
  android_ndk="${android_sdk}/ndk/${ndk_version}"
fi
[[ -n "$android_ndk" ]] ||
  die "Android NDK ${ndk_version} not found. Install it in Android Studio or pass --ndk PATH"
android_ndk="$(absolute_dir "$android_ndk")"
[[ -f "${android_ndk}/build/cmake/android.toolchain.cmake" ]] ||
  die "not a valid Android NDK directory: ${android_ndk}"

[[ -f "${android_sdk}/platforms/android-35/android.jar" ]] ||
  die "Android SDK platform 35 is missing. Install it with Android Studio's SDK Manager"

if [[ -z "${JAVA_HOME:-}" ]]; then
  if java_17_home="$(/usr/libexec/java_home -v 17 2>/dev/null)"; then
    export JAVA_HOME="$java_17_home"
  elif [[ -x "/opt/homebrew/opt/openjdk@17/bin/java" ]]; then
    export JAVA_HOME="/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home"
  elif [[ -x "/Applications/Android Studio.app/Contents/jbr/Contents/Home/bin/java" ]]; then
    export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
  else
    die "JDK 17 or newer is required. Install it with: brew install openjdk@17"
  fi
fi

java_version="$("${JAVA_HOME}/bin/java" -version 2>&1 | sed -n '1s/.*version "\([0-9]*\).*/\1/p')"
[[ "$java_version" =~ ^[0-9]+$ && "$java_version" -ge 17 ]] ||
  die "JDK 17 or newer is required, but JAVA_HOME points to Java ${java_version:-unknown}: ${JAVA_HOME}"

export ANDROID_HOME="$android_sdk"
export ANDROID_SDK_ROOT="$android_sdk"
export ANDROID_NDK_ROOT="$android_ndk"
export SKATE3_GAME_DATA_ROOT="$game_root"
export SKATE3_TITLE_UPDATE_PACKAGE="$title_update"

cd "$project_root"

if [[ ! -f third_party/rexglue-sdk/CMakeLists.txt ]]; then
  step "Initializing Git submodules"
  git submodule sync --recursive
  git submodule update --init --recursive --jobs 8
fi

step "Building native ARM64 libraries"
android/tools/build_android_libs.sh

step "Building Android APK"
(cd android && ./gradlew --no-daemon assembleDebug)

apk="${project_root}/android/app/build/outputs/apk/debug/app-debug.apk"
[[ -f "$apk" ]] || die "Gradle completed but the APK was not found at ${apk}"

mkdir -p "${project_root}/out"
friendly_apk="${project_root}/out/Skate3-Mobile-Android-debug.apk"
cp -f "$apk" "$friendly_apk"

if $install_apk; then
  if command -v adb >/dev/null 2>&1; then
    adb_bin="$(command -v adb)"
  elif [[ -x "${android_sdk}/platform-tools/adb" ]]; then
    adb_bin="${android_sdk}/platform-tools/adb"
  else
    die "adb is required for installation. Install Android SDK Platform-Tools"
  fi
  [[ -n "$("$adb_bin" devices | awk 'NR > 1 && $2 == "device" { print $1; exit }')" ]] ||
    die "no authorized Android device is connected"
  step "Installing APK"
  "$adb_bin" install -r "$friendly_apk"
fi

if $stage_game; then
  default_patch="${project_root}/out/build/android-release/game/default.xexp"
  webkit_patch="${project_root}/out/build/android-release/game/data/webkit/EAWebkit.xexp"
  [[ -f "$default_patch" && -f "$webkit_patch" ]] ||
    die "the generated Title Update patch files are missing"

  step "Copying your game data to /sdcard/skate3"
  "$adb_bin" shell mkdir -p /sdcard/skate3/data/webkit
  "$adb_bin" push "${game_root}/." /sdcard/skate3/
  "$adb_bin" push "$default_patch" /sdcard/skate3/default.xexp
  "$adb_bin" push "$webkit_patch" /sdcard/skate3/data/webkit/EAWebkit.xexp
fi

printf '\nBuild complete:\n  %s\n' "$friendly_apk"
printf '\nThe APK does not contain retail game data. See android/README.md for device setup.\n'
if $stage_game; then
  printf 'Game data copied. Launch the app and grant All files access when Android asks.\n'
fi
