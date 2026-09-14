#include <emu/emu.h>
#include <emu/Emulator.h>

namespace emu
{
  static Emulator emulator;

  DisplayBuffer *getDisplayBuffer()
  {
    return emulator.getDisplayBuffer();
  }

  void putDisplayBuffer(DisplayBuffer *buffer)
  {
    emulator.putDisplayBuffer(buffer);
  }

  int getEncoderValue()
  {
    return emulator.getEncoderValue();
  }

  bool isRearCardPresent()
  {
    return emulator.isRearCardPresent();
  }

  bool isFrontCardPresent()
  {
    return emulator.isFrontCardPresent();
  }

  // ---- Host (e.g. VCV Rack) entry points -----------------------------
  // Free-function wrappers around the singleton emulator instance so a
  // host bridge does not need the Emulator class definition (or SDL
  // headers) to drive the lifecycle.
  bool Emulator_initializeForHost(const char *configFile)
  {
    return emulator.initializeForHost(configFile);
  }

  void Emulator_pollHostEvents()
  {
    emulator.pollHostEvents();
  }

  void Emulator_shutdownForHost()
  {
    emulator.shutdownForHost();
  }

  bool Emulator_isReady()
  {
    return emulator.isReady();
  }

  bool Emulator_initializeHeadless(const char *configFile)
  {
    return emulator.initializeHeadless(configFile);
  }

  bool Emulator_initializeHeadlessWithPaths(const HostRuntimePaths &paths)
  {
    return emulator.initializeHeadless(paths);
  }

  void Emulator_hostRequestDisplayFrame()
  {
    emulator.hostRequestDisplayFrame();
  }

  bool Emulator_hostWaitGetDisplayFrame(uint8_t *mainOut, uint8_t *subOut,
                                        uint32_t timeoutMs)
  {
    return emulator.hostWaitGetDisplayFrame(mainOut, subOut, timeoutMs);
  }

  bool Emulator_hostGetDisplayFrame(uint8_t *mainOut, uint8_t *subOut)
  {
    return emulator.hostGetDisplayFrame(mainOut, subOut);
  }

  void Emulator_hostSetButton(uint32_t id, bool pressed)
  {
    emulator.hostSetButton(id, pressed);
  }

  void Emulator_hostTurnEncoder(int delta)
  {
    emulator.hostTurnEncoder(delta);
  }

  void Emulator_hostSetStorageSwitch(int position)
  {
    emulator.hostSetStorageSwitch(position);
  }

  void Emulator_hostSetModeSwitch(int position)
  {
    emulator.hostSetModeSwitch(position);
  }
}

#ifndef ER301_VCV_HOST
int main(int argc, char **argv)
{
  return emu::emulator.run(argc, argv);
}
#endif
