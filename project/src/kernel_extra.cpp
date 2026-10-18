// Copa 2014 - kernel exports not provided by the ReXGlue SDK.
#include <rex/hook.h>
#include <rex/logging.h>

// Xbox Live Vision camera: report "not connected" (ERROR_DEVICE_NOT_CONNECTED).
REX_STUB_RETURN(__imp__XUsbcamGetState, 0x48F)
REX_STUB_RETURN(__imp__XUsbcamSetConfig, 0x48F)
