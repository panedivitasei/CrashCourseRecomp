// Xbox Live Vision camera exports, stubbed to report a camera that is never
// connected.

#include <atomic>
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/types.h>

namespace {

rex::u32 XUsbcamCreate(rex::u32 buffer, rex::u32 buffer_size, rex::u32 unknown) {
  (void)buffer;
  (void)buffer_size;
  (void)unknown;
  return 0;  // X_STATUS_SUCCESS
}

rex::u32 XUsbcamGetState() { return 0; }  // not connected

// Live-camera calls are not expected with no camera, so the first few are
// logged and r3 is left as the caller set it.
void Unexpected(const char* name) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 4) REXKRNL_WARN("{} called with no camera connected", name);
}

}  // namespace

REX_EXPORT(__imp__XUsbcamCreate, XUsbcamCreate)
REX_EXPORT(__imp__XUsbcamGetState, XUsbcamGetState)

#define CC1_USBCAM_NO_CAMERA(name)                                   \
  extern "C" REX_FUNC(name) {                                        \
    (void)ctx;                                                       \
    (void)base;                                                      \
    Unexpected(#name);                                               \
  }                                                                  \
  static rex::ppc::detail::PPCFuncRegistrar _ppc_reg_##name(#name, &name);

CC1_USBCAM_NO_CAMERA(__imp__XUsbcamDestroy)
CC1_USBCAM_NO_CAMERA(__imp__XUsbcamReadFrame)
CC1_USBCAM_NO_CAMERA(__imp__XUsbcamSetCaptureMode)
CC1_USBCAM_NO_CAMERA(__imp__XUsbcamSetConfig)
CC1_USBCAM_NO_CAMERA(__imp__XUsbcamSetView)
