# Build Skate 3 Mobile without using commands

The current builder runs on Apple Silicon Macs. It creates a personal Android
APK from game files you legally own. The game and Title Update are not included
in this repository.

## What you need

1. An Apple Silicon Mac
2. Android Studio
3. Your fully extracted Skate 3 game folder
4. The Skate 3 Title Update 3 package
5. About 20 GB of free space for tools and temporary build files

The extracted game folder must contain both of these files:

```text
default.xex
data/webkit/EAWebkit.xex
```

## First-time setup

1. Install and open [Android Studio](https://developer.android.com/studio) once.
2. Double-click `Setup Build Tools.command` in the repository folder.
3. Approve the installation when asked.
4. If Android asks you to accept its SDK licenses in Terminal, read them and
   answer the prompts.

If macOS blocks a command file, Control-click it, choose **Open**, and confirm.

## Build the game

1. Double-click `Build Skate 3 Mobile.command`.
2. Read the legal reminder and choose **Continue**.
3. Select the extracted Skate 3 folder.
4. Select the Title Update 3 package.
5. Choose one of these options:

   - **Build only** creates the APK and selects it in Finder.
   - **Build, install, and copy game** also installs everything on a connected
     Android device. USB debugging or wireless debugging must already be on.

The first build takes the longest. Later builds reuse most of the compiled
files.

The finished APK is stored here:

```text
out/Skate3-Mobile-Android-debug.apk
```

If setup or the build fails, its log is selected in Finder. The logs are stored
at `out/noncoder-setup.log` and `out/noncoder-build.log`. They contain the exact
errors needed for troubleshooting.

## First launch

When Android asks for All files access, grant it and return to Skate 3 Mobile.
The application needs that permission to read your game data from
`/sdcard/skate3`.
