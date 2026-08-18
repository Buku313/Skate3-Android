#!/usr/bin/env bash
set -uo pipefail

export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:${PATH}"

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
default_game="${project_root}/game"
default_tu="${project_root}/TU_12K2276_000000C000000.00000000000O3"

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

continue_choice="$(osascript <<'APPLESCRIPT'
tell current application
  activate
  set answer to display dialog "This creates a personal Android APK from files you legally own. You will need an extracted Skate 3 folder and the Title Update 3 package." with title "Skate 3 Mobile Builder" buttons {"Cancel", "Continue"} default button "Continue" cancel button "Cancel"
  return button returned of answer
end tell
APPLESCRIPT
)" || exit 0

[[ "$continue_choice" == "Continue" ]] || exit 0

if [[ -f "${default_game}/default.xex" &&
      -f "${default_game}/data/webkit/EAWebkit.xex" ]]; then
  game_root="$default_game"
else
  game_root="$(osascript <<'APPLESCRIPT'
tell current application
  activate
  try
    return POSIX path of (choose folder with prompt "Choose your extracted Skate 3 folder. It must contain default.xex.")
  on error number -128
    return ""
  end try
end tell
APPLESCRIPT
)"
  [[ -n "$game_root" ]] || exit 0
fi

if [[ ! -f "${game_root}/default.xex" ||
      ! -f "${game_root}/data/webkit/EAWebkit.xex" ]]; then
  show_message "Wrong folder" "That folder is not a complete extracted Skate 3 game. Choose the folder containing default.xex and data/webkit/EAWebkit.xex."
  exit 1
fi

if [[ -f "$default_tu" ]]; then
  title_update="$default_tu"
else
  title_update="$(osascript <<'APPLESCRIPT'
tell current application
  activate
  try
    return POSIX path of (choose file with prompt "Choose your Skate 3 Title Update 3 package.")
  on error number -128
    return ""
  end try
end tell
APPLESCRIPT
)"
  [[ -n "$title_update" ]] || exit 0
fi

action="$(osascript <<'APPLESCRIPT'
tell current application
  activate
  set answer to display dialog "Choose Build only to create the APK. Choose Build, install, and copy game if an Android device is connected with USB or wireless debugging." with title "Skate 3 Mobile Builder" buttons {"Cancel", "Build only", "Build, install, and copy game"} default button "Build only" cancel button "Cancel"
  return button returned of answer
end tell
APPLESCRIPT
)" || exit 0

build_args=(--game-dir "$game_root" --title-update "$title_update")
if [[ "$action" == "Build, install, and copy game" ]]; then
  build_args+=(--stage-game)
fi

mkdir -p "${project_root}/out"
log_file="${project_root}/out/noncoder-build.log"

printf '\nSkate 3 Mobile Builder\n'
printf 'The first build may take a while. You can watch its progress below.\n\n'

set +e
"${project_root}/build-android.sh" "${build_args[@]}" 2>&1 | tee "$log_file"
build_status=${PIPESTATUS[0]}
set -e

apk="${project_root}/out/Skate3-Mobile-Android-debug.apk"
if [[ $build_status -eq 0 && -f "$apk" ]]; then
  open -R "$apk"
  if [[ "$action" == "Build, install, and copy game" ]]; then
    show_message "Finished" "Skate 3 Mobile was built, installed, and copied to your Android device. Launch it and grant All files access when Android asks."
  else
    show_message "Finished" "Skate 3 Mobile was built successfully. The APK is selected in Finder."
  fi
else
  open -R "$log_file"
  show_message "Build failed" "The builder stopped before finishing. The build log is selected in Finder. Run Setup Build Tools.command if a required tool is missing."
  exit "$build_status"
fi
