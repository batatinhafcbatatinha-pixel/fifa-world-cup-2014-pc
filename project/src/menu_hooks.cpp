// The game's own menu leads to the PC settings. The home screen's header line "SIGN IN TO EA
// SERVERS" (START) runs a flow that asks for the Xbox LIVE sign-in UI, a console service the
// PC does not have: that request opens the PC settings instead, and pow_hooks.cpp (in the UI
// module) renames the line. The flow then shows its "connecting to the EA servers... (A)
// Cancel" box; when the PC settings close, the pad reports one press of A, which cancels it
// the way the player would. copa2014-pc.ini [opcoes] menu_no_jogo=0 restores the original.
#include <atomic>
#include <chrono>
#include <cstdlib>

#include <rex/hook.h>
#include <rex/logging.h>

#include "native/guest_device.h"
#include "pc_settings.h"

namespace {

using Clock = std::chrono::steady_clock;

// 1 while the PC settings asked for from the game's line have not been closed yet.
std::atomic<int> g_entry{0};
std::atomic<int64_t> g_requested{0};
// While now < g_press_until, the pad holds A.
std::atomic<int64_t> g_press_until{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

}  // namespace

REX_EXTERN(__imp__sub_824F9340);
REX_EXTERN(__imp__sub_83A2DA90);

// sub_824F9340(this, user, online_only): the platform layer's "show the sign-in UI"
// (XShowSigninUI); 0 = shown. Called from the EA servers flow (return address 0x824AED70)
// only when the player asks for it; the flow also starts by itself at boot without reaching it.
REX_HOOK_RAW(sub_824F9340) {
  if (!pcs::GameMenuEntry() || uint32_t(ctx.lr) != 0x824AED70u) {
    __imp__sub_824F9340(ctx, base);
    return;
  }
  REXLOG_INFO("[menu] EA servers line chosen: opening the PC settings");
  g_requested = NowMs();
  g_entry = 1;
  pcs::RequestMenu();
  ctx.r3.u64 = 0;
}

// sub_83A2DA90(user, state): XInputGetState. state+4 is the buttons (big-endian), A = 0x1000.
REX_HOOK_RAW(sub_83A2DA90) {
  const uint32_t state = ctx.r4.u32;
  __imp__sub_83A2DA90(ctx, base);
  // The game does not read the pad while the menu holds it, so the first read after the
  // request (once the menu is no longer open) is the moment it closed.
  if (g_entry.load() && !pcs::MenuOpen() && NowMs() - g_requested.load() > 250) {
    g_entry = 0;
    g_press_until = NowMs() + 150;
    REXLOG_INFO("[menu] PC settings closed: pressing A to cancel the EA servers box");
  }
  if (ctx.r3.u32 == 0 && state && NowMs() < g_press_until.load()) {
    uint8_t* buttons = copa::GuestPtr(base, state + 4);
    buttons[0] |= 0x10;  // A (0x1000), high byte first
  }
}
