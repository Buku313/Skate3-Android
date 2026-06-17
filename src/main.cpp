// skate3 - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#include "skate3_app_common.h"
#include <skate3_version.h>

#include <string>
#include <string_view>

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
    return "Skate 3 " SKATE3_BUILD_TITLE;
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Skate3PureApp>(
        new Skate3PureApp(ctx, "skate3", skate3_PPCImageConfig));
  }
};

REX_DEFINE_APP(skate3, Skate3PureApp::Create)
