#pragma once

#include <emu/HostRuntimePaths.h>
#include <hal/display.h>
#include <stdint.h>

namespace emu
{
  // Thread-safe.
  void putDisplayBuffer(DisplayBuffer *buffer);
  DisplayBuffer *getDisplayBuffer();
  int getEncoderValue();
  bool isRearCardPresent();
  bool isFrontCardPresent();

  // Host (e.g. VCV Rack) lifecycle wrappers around the emulator singleton.
  // See emu/Emulator.h for semantics.  All must be called from the host
  // UI/main thread.
  bool Emulator_initializeForHost(const char *configFile);
  void Emulator_pollHostEvents();
  void Emulator_shutdownForHost();
  bool Emulator_isReady();

  // Headless host mode (no SDL window/video/event loop). The host owns
  // audio I/O, controls, and display presentation. See emu/Emulator.h.
  bool Emulator_initializeHeadless(const char *configFile);
  bool Emulator_initializeHeadlessWithPaths(const HostRuntimePaths &paths);
  void Emulator_hostRequestDisplayFrame();
  bool Emulator_hostWaitGetDisplayFrame(uint8_t *mainOut, uint8_t *subOut,
                                        uint32_t timeoutMs);
  bool Emulator_hostGetDisplayFrame(uint8_t *mainOut, uint8_t *subOut);
  void Emulator_hostSetButton(uint32_t id, bool pressed);
  void Emulator_hostTurnEncoder(int delta);
  void Emulator_hostSetStorageSwitch(int position); // 1=up 2=mid 3=down
  void Emulator_hostSetModeSwitch(int position);    // 1=up 2=mid 3=down
}