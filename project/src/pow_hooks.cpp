// Compiled into the UI module (powdllzf): renames the home screen's "SIGN IN TO EA SERVERS"
// line, which leads to the PC settings (menu_hooks.cpp). The localized text is decoded from the
// game's string database into a buffer and copied with std::string::assign (sub_89741210); the
// source range is rewritten there, padded with spaces to the same length so nothing else moves.
#include <cstring>

#include <rex/hook.h>
#include "native/guest_device.h"
#include "pc_settings.h"

namespace {

struct Rename {
  const char* from;  // as stored by the game (UTF-8)
  const char* to;    // at most as long, in bytes
};

// data/loc/locale.big has eng_us and por_br.
const Rename kRenames[] = {
    {"SIGN IN TO EA SERVERS", "PC SETTINGS"},
    {"ENTRAR NOS SERVIDORES DA EA", "CONFIGURA\xC3\x87\xC3\x95" "ES DO PC"},
};

}  // namespace

REX_EXTERN(__imp__sub_89741210);

// sub_89741210(string, begin, end): std::string::assign from a character range.
REX_HOOK_RAW(sub_89741210) {
  const uint32_t begin = ctx.r4.u32, end = ctx.r5.u32;
  const uint32_t length = end - begin;
  if (begin && length >= 16 && length <= 40 && pcs::GameMenuEntry()) {
    char* text = reinterpret_cast<char*>(copa::GuestPtr(base, begin));
    for (const Rename& r : kRenames) {
      size_t n = std::strlen(r.from);
      if (n == length && std::memcmp(text, r.from, n) == 0) {
        size_t m = std::strlen(r.to);
        std::memcpy(text, r.to, m);
        std::memset(text + m, ' ', n - m);
        break;
      }
    }
  }
  __imp__sub_89741210(ctx, base);
}
