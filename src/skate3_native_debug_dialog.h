#pragma once

// F12 native-render debug menu: live checkboxes for every native renderer
// subsystem (all hot-reload cvars), used to bisect visual regressions
// (flicker, wrong shading) in-game without rebuilds.

#include <rex/ui/imgui_dialog.h>

namespace skate3 {

class NativeDebugDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit NativeDebugDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  void Show();
  void Hide();
  void Toggle();
  bool visible() const { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  bool visible_ = false;
};

}  // namespace skate3
