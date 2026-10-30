#include "platform/native_byteorder.h"
// Copa 2014 - kernel exports not provided by the ReXGlue SDK.
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>

#include "native/guest_device.h"

// Xbox Live Vision camera: report "not connected" (ERROR_DEVICE_NOT_CONNECTED).
REX_STUB_RETURN(__imp__XUsbcamGetState, 0x48F)
REX_STUB_RETURN(__imp__XUsbcamSetConfig, 0x48F)

// Voice (XHV) engine buffer size, sub_8394EC28(user, count, &size, &handle): it reports
// count * 820 bytes only after XamCreateEnumeratorHandle (app 0xFA) succeeds, which the SDK
// does not implement. Its caller (sub_839A6678) allocates size + 48 bytes regardless and clears
// a halfword in each of the 12 entries (824 bytes apart): with size 0 that overflows the heap
// and corrupted a neighbouring object (intermittent crash at boot in sub_836CE548). Report the
// size the console would; the engine still fails as before.
REX_EXTERN(__imp__sub_8394EC28);
REX_HOOK_RAW(sub_8394EC28) {
  const uint32_t count = ctx.r4.u32, size_ptr = ctx.r5.u32;
  __imp__sub_8394EC28(ctx, base);
  if (ctx.r3.u32 != 0 && size_ptr && copa::LoadBE32(copa::GuestPtr(base, size_ptr)) == 0) {
    const uint32_t size = native_host::ByteSwap32(count * 820);
    std::memcpy(copa::GuestPtr(base, size_ptr), &size, 4);
    static bool logged = false;
    if (!logged) REXLOG_INFO("copa: XHV engine unavailable; voice buffer size set to {} bytes", count * 820);
    logged = true;
  }
}
