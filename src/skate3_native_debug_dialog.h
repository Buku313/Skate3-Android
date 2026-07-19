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

// Tiny top-right corner readout of which renderer produced the last
// presented frame: NATIVE (native scene renderer) or EMULATED (Xenos
// GPU emulation; menus/loading yields, F5 off). Input-transparent, no
// continuous repaint (updates ride the guest frame paints); off by
// default, shown via skate3_native_render_mode_indicator (hot).
class RenderModeIndicator final : public rex::ui::ImGuiDialog {
 public:
  explicit RenderModeIndicator(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  bool WantsContinuousRepaint() const override { return false; }
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace skate3
