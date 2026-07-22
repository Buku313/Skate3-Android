// skate3 - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#include "skate3_app_common.h"
#include <skate3_version.h>

#include <string>
#include <string_view>

// NVIDIA Optimus / AMD PowerXpress discrete-GPU opt-in. The drivers only read
// these from the executable's export table; the copies in rexruntime.dll are
// invisible to them, which left hybrid-graphics laptops rendering on the
// integrated GPU. They must live here, in the exe image itself.
#if defined(_WIN32)
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) unsigned long AmdPowerXpressRequestHighPerformance = 1;
}
#endif

class Skate3PureApp : public Skate3BaseApp {
 public:
  using Skate3BaseApp::Skate3BaseApp;

  std::string_view GetBuildTitle() const override {
    return SKATE3_BUILD_TITLE;
  }

  std::string_view GetBuildStamp() const override {
    return SKATE3_BUILD_STAMP;
  }

  std::string GetWindowTitle() const override {
#if defined(__APPLE__)
    // Match the macOS bundle name so the title bar and the .app agree.
    return "skate3recomp " SKATE3_BUILD_TITLE;
#else
    return "Skate 3 " SKATE3_BUILD_TITLE;
#endif
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Skate3PureApp>(
        new Skate3PureApp(ctx, "skate3", skate3_PPCImageConfig));
  }
};

REX_DEFINE_APP(skate3, Skate3PureApp::Create)
