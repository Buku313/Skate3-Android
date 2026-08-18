# Set up Skate 3 Mobile without using commands

## Easiest route: Android only

You do not need to build anything for ordinary setup:

1. Put an ISO dumped from your own supported Skate 3 Xbox 360 copy on your
   Android device, SD card, or USB drive.
2. Install and open the Skate 3 Mobile APK.
3. Tap **Select My Skate 3 ISO** and choose that ISO in Android's file picker.
4. Keep the app open while it extracts and verifies about 6.0 GiB of game data.
5. The app downloads and verifies Title Update 3 automatically.
6. Tap **Play Skate 3**.

The original skater is the default. To use the optional Seiyu Paradise Penguin
Mod, tap the mod-files button and choose your user-supplied `base.obj` and
`texture_diffuse.png`. In game, press **RB + Start** and use **Mods > Playable
Character** to switch between Seiyu and the original skater.

No laptop, ISO extraction app, file-manager folder setup, or command line is
needed. Your ISO is never modified or uploaded. Keep about 8 GiB free for the
installed game. If the ISO is also on internal storage, allow about 15 GiB total
until you move or delete the ISO.

Keep the original ISO. Android deletes the app-owned extracted game when the app
is uninstalled.

## Optional: make a personal custom APK on Mac

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

## First launch after developer staging

If the Mac builder copied game files to `/sdcard/skate3`, grant All files access
when Android asks and return to Skate 3 Mobile. Phone-only installations use
app-specific storage and do not need that permission.
