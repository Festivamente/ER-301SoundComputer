#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Firmware-facing API exposed to Lua through app.cpp.swig.
// status(): 0 = waiting/idle, 1 = file selected, 2 = dialog cancelled.
bool HostFileDialog_request(const char *initialPath, const char *filterSpec);
int HostFileDialog_status(void);
const char *HostFileDialog_takeResult(void);
void HostFileDialog_reset(void);

#ifdef __cplusplus
}

namespace emu
{
  // Host-facing half. A request is consumed on Rack's UI thread, which opens
  // the native OS dialog and then completes it with a path or nullptr.
  bool HostFileDialog_takeRequest(const char **initialPath, const char **filterSpec);
  void HostFileDialog_complete(const char *path);
}
#endif
