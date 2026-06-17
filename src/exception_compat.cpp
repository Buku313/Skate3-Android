#include "skate3_init.h"

#include <cstdint>

// The guest C runtime dispatches structured-exception guards through a
// trampoline pointer that real hardware installs during CRT startup. The
// recompiled runtime never installs it, so the generated guard function would
// return with r3 untouched (its argument: a non-zero stack buffer), which the
// 17 call sites interpret as "an exception occurred" and silently skip their
// guarded work. Force the setjmp-style "direct return" result (r3 = 0) when
// the trampoline is absent.
//
// Retail 3.0.0.0: guard at 0x82F44E40, trampoline at 0x83092CC0.
// Title update 3.0.3.0: guard at 0x82F6FAA0, trampoline at 0x830EF8C0.
#if SKATE3_HAS_TITLE_UPDATE
#define SKATE3_EXCEPTION_GUARD_FUNC sub_82F6FAA0
#define SKATE3_EXCEPTION_GUARD_IMP __imp__sub_82F6FAA0
constexpr uint32_t kExceptionTrampoline = 0x830EF8C0u;
#else
#define SKATE3_EXCEPTION_GUARD_FUNC sub_82F44E40
#define SKATE3_EXCEPTION_GUARD_IMP __imp__sub_82F44E40
constexpr uint32_t kExceptionTrampoline = 0x83092CC0u;
#endif

extern "C" REX_FUNC(SKATE3_EXCEPTION_GUARD_IMP);

extern "C" REX_FUNC(SKATE3_EXCEPTION_GUARD_FUNC) {
  const uint32_t trampoline = REX_LOAD_U32(kExceptionTrampoline);
  if (trampoline == 0u) {
    ctx.r3.u64 = 0;
    return;
  }

  SKATE3_EXCEPTION_GUARD_IMP(ctx, base);
}
