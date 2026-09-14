#include <hal/reboot.h>
#include <hal/log.h>

#include <atomic>

namespace
{
  std::atomic<bool> gHostRebootRequested{false};
}

extern "C" void reboot()
{
#ifdef ER301_VCV_HOST
  gHostRebootRequested.store(true, std::memory_order_release);
  logInfo("ER-301 host reboot requested.");
#else
  // The standalone emulator should never attempt to reboot the host OS.
  logInfo("ER-301 emulator reboot requested.");
#endif
}

namespace emu
{
  bool HostReboot_requested()
  {
    return gHostRebootRequested.load(std::memory_order_acquire);
  }

  bool HostReboot_consume()
  {
    return gHostRebootRequested.exchange(false, std::memory_order_acq_rel);
  }
}
