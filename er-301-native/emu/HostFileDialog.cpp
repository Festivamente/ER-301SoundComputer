#include "HostFileDialog.h"

#include <algorithm>
#include <mutex>
#include <string>

namespace
{
  enum State
  {
    IDLE = 0,
    REQUESTED,
    OPEN,
    SELECTED,
    CANCELLED
  };

  std::mutex g_mutex;
  State g_state = IDLE;
  std::string g_initialPath;
  std::string g_filterSpec;
  std::string g_result;

  std::string normalizePath(const char *path)
  {
    std::string result = path ? path : "";
#ifdef _WIN32
    // ER-301's Lua Path module uses '/' on every platform. Win32 accepts
    // forward slashes, so normalize the native Explorer result at the bridge.
    std::replace(result.begin(), result.end(), '\\', '/');
#endif
    return result;
  }
}

extern "C" bool HostFileDialog_request(const char *initialPath,
                                         const char *filterSpec)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_state != IDLE)
    return false;

  g_initialPath = normalizePath(initialPath);
  g_filterSpec = filterSpec ? filterSpec : "";
  g_result.clear();
  g_state = REQUESTED;
  return true;
}

extern "C" int HostFileDialog_status(void)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_state == SELECTED)
    return 1;
  if (g_state == CANCELLED)
    return 2;
  return 0;
}

extern "C" const char *HostFileDialog_takeResult(void)
{
  static thread_local std::string result;
  std::lock_guard<std::mutex> lock(g_mutex);
  result = (g_state == SELECTED) ? g_result : std::string();
  g_initialPath.clear();
  g_filterSpec.clear();
  g_result.clear();
  g_state = IDLE;
  return result.c_str();
}

extern "C" void HostFileDialog_reset(void)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  // Do not invalidate a dialog that is already open on the Rack UI thread.
  // It will be completed normally when the user closes it.
  if (g_state == OPEN)
    return;
  g_initialPath.clear();
  g_filterSpec.clear();
  g_result.clear();
  g_state = IDLE;
}

namespace emu
{
  bool HostFileDialog_takeRequest(const char **initialPath,
                                  const char **filterSpec)
  {
    static thread_local std::string initialPathStorage;
    static thread_local std::string filterSpecStorage;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state != REQUESTED || !initialPath || !filterSpec)
      return false;

    initialPathStorage = g_initialPath;
    filterSpecStorage = g_filterSpec;
    *initialPath = initialPathStorage.c_str();
    *filterSpec = filterSpecStorage.c_str();
    g_state = OPEN;
    return true;
  }

  void HostFileDialog_complete(const char *path)
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state != OPEN)
      return;

    if (path && path[0] != '\0')
    {
      g_result = normalizePath(path);
      g_state = SELECTED;
    }
    else
    {
      g_result.clear();
      g_state = CANCELLED;
    }
  }
}
