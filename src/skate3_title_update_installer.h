#pragma once

#include <filesystem>
#include <functional>

#include <rex/rex_app.h>

namespace skate3 {

// True when both title update payloads (default.xexp and
// data/webkit/EAWebkit.xexp) are staged in game_root and match the pinned
// SHA-256 hashes the recompilation was generated from.
bool IsTitleUpdateInstalled(const std::filesystem::path& game_root);

// Stages the title update payloads into game_root from a local source file:
// either the TU STFS package (CON/LIVE/PIRS container) or a raw .xexp payload.
bool StageTitleUpdateFromFile(const std::filesystem::path& source,
                              const std::filesystem::path& game_root, std::string& error);

void ShowTitleUpdateInstallWizard(rex::ui::ImGuiDrawer* drawer, rex::PathConfig runtime_paths,
                                  std::function<void(rex::PathConfig)> complete);
bool RunTitleUpdateInstallWizardBlocking(rex::ui::WindowedAppContext& app_context,
                                         rex::ui::Window* window,
                                         rex::ui::ImGuiDrawer* drawer,
                                         rex::PathConfig runtime_paths,
                                         rex::PathConfig& installed_paths);

}  // namespace skate3
