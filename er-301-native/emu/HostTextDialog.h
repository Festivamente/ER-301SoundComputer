#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Firmware-facing API exposed to Lua through app.cpp.swig.
// status(): 0 = waiting/idle, 1 = text submitted, 2 = dialog cancelled.
bool HostTextDialog_request(const char *message, const char *initialText,
                            bool extended);
int HostTextDialog_status(void);
const char *HostTextDialog_takeResult(void);
void HostTextDialog_reset(void);

#ifdef __cplusplus
}

namespace emu
{
  // Host-facing half. Each isolated ER-301 engine owns one request state.
  // Rack consumes the request on its UI thread and completes it with text or
  // nullptr for cancellation.
  bool HostTextDialog_takeRequest(const char **message,
                                  const char **initialText,
                                  int *extended);
  void HostTextDialog_complete(const char *text);
}
#endif
