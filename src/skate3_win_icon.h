#pragma once

#include <filesystem>

namespace skate3 {

// Application icon sourced from the USER'S OWN game files at runtime; the
// shipped executable and the repository deliberately contain no EA artwork
// (same clean-distribution rule as every other game asset).
//
// Windows only (no-op elsewhere). Two halves, both fed by the 64x64 title
// tile PNG embedded in game/nxeart (the Xbox dashboard-art package):
//  - window/taskbar/Alt-Tab icon: WM_SETICON on the game window, every run;
//  - Explorer file icon: a one-time self-patch that writes a MAINICON
//    resource group into the exe ON DISK (the running image is rename-and-
//    copy swapped since it is write-locked). Idempotent: subsequent runs see
//    the resource and skip; overwriting the exe with an update simply
//    re-patches on the next run.
//
// Runs on a background thread; failures only log (the icon stays default).
void ApplyGameIconFromGameData(const std::filesystem::path& game_data_root,
                               void* native_window_handle);

}  // namespace skate3
