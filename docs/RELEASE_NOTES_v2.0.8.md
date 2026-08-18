# Skate 3 Mobile v2.0.8

This release makes Skate 3 Mobile practical on touchscreen phones and easier
to keep updated.

## Added

- Multitouch Xbox-style controls over the game.
- Two analog sticks, D-pad, ABXY, triggers, bumpers, Start, Back, L3, and R3.
- A persistent TOUCH or HIDE tab that remembers your preference.
- Automatic update checks in the phone launcher.
- Verified APK downloads with a one-tap handoff to Android's installer.

## Input behavior

- Touch input merges into player one instead of replacing a connected pad.
- The overlay starts hidden when a physical controller is detected.
- It starts visible on touchscreen-only phones.
- Touch input is cleared whenever the game is paused or the overlay is hidden.

Android requires the user to approve each APK installation. Existing v2.0.7
users need to install v2.0.8 manually once. Later builds can be downloaded from
inside the app. Updating does not remove locally extracted game files.

The APK does not include retail Skate 3 game data. Import an ISO dumped from
your own copy using the phone-only installer.
