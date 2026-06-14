#include "skate3_demo_path.h"

#include "generated/skate3_init.h"

#include <atomic>
#include <cstdint>
#include <limits>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/kernel/xam/input_injection.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

REXCVAR_DEFINE_BOOL(skate3_demo_path, false, "Skate 3",
                    "Probe and automate the boot path to gameplay");
REXCVAR_DEFINE_BOOL(skate3_demo_path_probe, false, "Skate 3",
                    "Log Skate 3 boot/frontend states used by the demo path");

namespace skate3::demo_path {
namespace {

constexpr uint32_t kFrontEndStatePressStart = 24;
constexpr uint32_t kFrontEndStateLanguageSelect = 47;

std::atomic<uint32_t> g_last_requested_state{0};
std::atomic<bool> g_seen_language_update{false};
std::atomic<uint32_t> g_last_language_select_event{std::numeric_limits<uint32_t>::max()};
std::atomic<uint32_t> g_last_press_start_event{std::numeric_limits<uint32_t>::max()};
std::atomic<uint32_t> g_automation_stage{0};
std::atomic<bool> g_skip_intro_movie{false};
std::atomic<bool> g_logged_intro_movie_skip{false};
std::atomic<bool> g_f10_poll_was_down{false};

bool ProbeEnabled() {
  return rex::cvar::Query<bool>("skate3_demo_path") ||
         rex::cvar::Query<bool>("skate3_demo_path_probe");
}

bool AutomationEnabled() {
  return rex::cvar::Query<bool>("skate3_demo_path");
}

const char* KnownFrontEndStateName(uint32_t state_id) {
  switch (state_id) {
    case kFrontEndStatePressStart:
      return "press-start";
    case kFrontEndStateLanguageSelect:
      return "language-select";
    default:
      return "unknown";
  }
}

void LogBootFlowEventChange(std::atomic<uint32_t>& last_event, const char* name, uint32_t event,
                            uint32_t state_this) {
  if (!ProbeEnabled()) {
    return;
  }

  uint32_t previous = last_event.load(std::memory_order_relaxed);
  while (previous != event) {
    if (last_event.compare_exchange_weak(previous, event, std::memory_order_relaxed)) {
      REXLOG_INFO("Skate 3 demo path: {} event={} this=0x{:08X}", name, event, state_this);
      return;
    }
  }
}

void QueueLanguageAcceptIfNeeded() {
  if (!AutomationEnabled()) {
    return;
  }

  uint32_t expected = 0;
  if (!g_automation_stage.compare_exchange_strong(expected, 1, std::memory_order_relaxed)) {
    return;
  }

  rex::kernel::xam::QueueSyntheticInput(rex::input::X_INPUT_GAMEPAD_A, 8);
  REXLOG_INFO("Skate 3 demo path: queued language-select A pulse");
}

void EnableTitleStartAutoTapIfNeeded(const char* reason) {
  if (!AutomationEnabled()) {
    return;
  }

  uint32_t stage = g_automation_stage.load(std::memory_order_relaxed);
  while (stage < 2) {
    if (g_automation_stage.compare_exchange_weak(stage, 2, std::memory_order_relaxed)) {
      rex::kernel::xam::SetSyntheticAutoTap(rex::input::X_INPUT_GAMEPAD_START, true);
      REXLOG_INFO("Skate 3 demo path: enabled press-start auto-tap ({})", reason);
      return;
    }
  }
}

void EnableIntroMovieSkipIfNeeded() {
  if (!AutomationEnabled()) {
    return;
  }

  bool expected = false;
  if (g_skip_intro_movie.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
    REXLOG_INFO("Skate 3 demo path: enabled intro movie completion override");
  }
}

void PollF10Marker() {
#if defined(_WIN32)
  if (!ProbeEnabled()) {
    return;
  }

  const bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
  const bool was_down = g_f10_poll_was_down.exchange(f10_down, std::memory_order_relaxed);
  if (f10_down && !was_down) {
    REXLOG_WARN("Skate 3 demo path: polled F10 milestone marker");
  }
#endif
}

extern "C" REX_FUNC(Skate3DemoPath_SetFrontEndStateHook) {
  const uint32_t manager = ctx.r3.u32;
  const uint32_t state_id = ctx.r4.u32;
  const uint32_t mode = ctx.r5.u32;
  const uint32_t caller_lr = ctx.lr;

  if (ProbeEnabled()) {
    g_last_requested_state.store(state_id, std::memory_order_relaxed);
    REXLOG_INFO(
        "Skate 3 demo path: FE SetState state={} ({}) mode={} manager=0x{:08X} lr=0x{:08X}",
        state_id, KnownFrontEndStateName(state_id), mode, manager, caller_lr);
  }

  sub_82D0AFA0(ctx, base);
}

extern "C" REX_FUNC(Skate3DemoPath_LanguageSelectStateHook) {
  PollF10Marker();
  LogBootFlowEventChange(g_last_language_select_event, "BootFlow LanguageSelectState",
                         ctx.r4.u32, ctx.r3.u32);
  if (ctx.r4.u32 == 4) {
    EnableIntroMovieSkipIfNeeded();
  }

  sub_826FDD70(ctx, base);
}

extern "C" REX_FUNC(Skate3DemoPath_ShowPressStartModeHook) {
  PollF10Marker();
  LogBootFlowEventChange(g_last_press_start_event, "BootFlow ShowPressStartMode", ctx.r4.u32,
                         ctx.r3.u32);
  if (ctx.r4.u32 == 1) {
    g_skip_intro_movie.store(false, std::memory_order_relaxed);
    EnableTitleStartAutoTapIfNeeded("press-start state");
  }

  sub_826FE1D8(ctx, base);
}

extern "C" REX_FUNC(Skate3DemoPath_LanguageSelectUpdateHook) {
  PollF10Marker();
  if (ProbeEnabled()) {
    bool expected = false;
    if (g_seen_language_update.compare_exchange_strong(expected, true,
                                                       std::memory_order_relaxed)) {
      REXLOG_INFO(
          "Skate 3 demo path: FrontEndState_LanguageSelect::OnUpdate first seen "
          "dt={} this=0x{:08X}",
          ctx.r4.u32, ctx.r3.u32);
      QueueLanguageAcceptIfNeeded();
    }
  }

  sub_82639400(ctx, base);
}

}  // namespace

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher) {
  if (!dispatcher || !ProbeEnabled()) {
    return;
  }

  dispatcher->SetFunction(0x82D0AFA0, &Skate3DemoPath_SetFrontEndStateHook);
  dispatcher->SetFunction(0x826FDD70, &Skate3DemoPath_LanguageSelectStateHook);
  dispatcher->SetFunction(0x826FE1D8, &Skate3DemoPath_ShowPressStartModeHook);
  dispatcher->SetFunction(0x82639400, &Skate3DemoPath_LanguageSelectUpdateHook);
  REXLOG_INFO("Skate 3 demo path: frontend probe hooks installed");
}

bool ShouldForceIntroMovieComplete() {
  if (!AutomationEnabled() || !g_skip_intro_movie.load(std::memory_order_relaxed)) {
    return false;
  }

  bool expected = false;
  if (g_logged_intro_movie_skip.compare_exchange_strong(expected, true,
                                                       std::memory_order_relaxed)) {
    REXLOG_INFO("Skate 3 demo path: forcing frontend intro movie complete");
  }
  return true;
}

}  // namespace skate3::demo_path
