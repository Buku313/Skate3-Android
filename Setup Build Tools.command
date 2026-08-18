#!/usr/bin/env bash
set -euo pipefail

export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:${PATH}"

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "${project_root}/out"
setup_log="${project_root}/out/noncoder-setup.log"

exec > >(tee "$setup_log") 2>&1

show_message() {
  osascript - "$1" "$2" <<'APPLESCRIPT'
on run argv
  tell current application
    activate
    display dialog (item 2 of argv) with title (item 1 of argv) buttons {"OK"} default button "OK"
  end tell
end run
APPLESCRIPT
}

show_setup_error() {
  status=$?
  trap - ERR
  set +e
  open -R "$setup_log"
  show_message "Setup failed" "Setup could not finish. The setup log is selected in Finder and contains the exact error."
  exit "$status"
}

trap show_setup_error ERR

[[ "$(uname -s)" == "Darwin" && "$(uname -m)" == "arm64" ]] || {
  show_message "Unsupported Mac" "The current builder requires an Apple Silicon Mac."
  exit 1
}

if ! command -v brew >/dev/null 2>&1; then
  open "https://brew.sh/"
  show_message "Install Homebrew" "Homebrew is required. Its official installation page is open. Install Homebrew, then double-click this setup file again."
  exit 1
fi

confirm="$(osascript <<'APPLESCRIPT'
tell current application
  activate
  set answer to display dialog "This will use Homebrew to install CMake, Ninja, LLVM, and OpenJDK 17. It will then install the required Android SDK components if Android Studio is already set up." with title "Skate 3 Mobile Setup" buttons {"Cancel", "Install"} default button "Install" cancel button "Cancel"
  return button returned of answer
end tell
APPLESCRIPT
)" || exit 0

[[ "$confirm" == "Install" ]] || exit 0

brew install cmake ninja llvm openjdk@17

android_sdk="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
if [[ -z "$android_sdk" && -d "${HOME}/Library/Android/sdk" ]]; then
  android_sdk="${HOME}/Library/Android/sdk"
fi

if [[ -z "$android_sdk" || ! -d "$android_sdk" ]]; then
  open "https://developer.android.com/studio"
  show_message "Android Studio needed" "The Android SDK was not found. Install and open Android Studio once, then double-click this setup file again."
  exit 1
fi

sdkmanager="${android_sdk}/cmdline-tools/latest/bin/sdkmanager"
if [[ ! -x "$sdkmanager" ]]; then
  if [[ -d "${android_sdk}/cmdline-tools" ]]; then
    sdkmanager="$(find "${android_sdk}/cmdline-tools" -path '*/bin/sdkmanager' -type f -print -quit 2>/dev/null || true)"
  else
    sdkmanager=""
  fi
fi
if [[ -z "$sdkmanager" ]]; then
  open -a "Android Studio" 2>/dev/null || true
  show_message "SDK tools needed" "In Android Studio, open SDK Manager, choose SDK Tools, enable Android SDK Command-line Tools, and apply the change. Then run this setup again."
  exit 1
fi

printf '\nAndroid may ask you to accept its SDK licenses in this Terminal window.\n\n'
"$sdkmanager" "platforms;android-35" "ndk;27.2.12479018" "platform-tools"

show_message "Setup complete" "The Android build tools are ready. Double-click Build Skate 3 Mobile.command to create your APK."
open -R "${project_root}/Build Skate 3 Mobile.command"
