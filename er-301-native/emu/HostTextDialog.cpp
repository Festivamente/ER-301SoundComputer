#include "HostTextDialog.h"

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
  std::string g_message;
  std::string g_initialText;
  std::string g_result;
  bool g_extended = false;
}

extern "C" bool HostTextDialog_request(const char *message,
                                         const char *initialText,
                                         bool extended)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_state != IDLE)
    return false;

  g_message = message ? message : "Enter text";
  g_initialText = initialText ? initialText : "";
  g_result.clear();
  g_extended = extended;
  g_state = REQUESTED;
  return true;
}

extern "C" int HostTextDialog_status(void)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_state == SELECTED)
    return 1;
  if (g_state == CANCELLED)
    return 2;
  return 0;
}

extern "C" const char *HostTextDialog_takeResult(void)
{
  static thread_local std::string result;
  std::lock_guard<std::mutex> lock(g_mutex);
  result = (g_state == SELECTED) ? g_result : std::string();
  g_message.clear();
  g_initialText.clear();
  g_result.clear();
  g_extended = false;
  g_state = IDLE;
  return result.c_str();
}

extern "C" void HostTextDialog_reset(void)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  // Do not invalidate an already-open Rack popup. Its destruction/submit path
  // will complete the request and restore the state normally.
  if (g_state == OPEN)
    return;
  g_message.clear();
  g_initialText.clear();
  g_result.clear();
  g_extended = false;
  g_state = IDLE;
}

namespace emu
{
  bool HostTextDialog_takeRequest(const char **message,
                                  const char **initialText,
                                  int *extended)
  {
    static thread_local std::string requestMessage;
    static thread_local std::string requestInitial;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state != REQUESTED)
      return false;

    requestMessage = g_message;
    requestInitial = g_initialText;
    if (message)
      *message = requestMessage.c_str();
    if (initialText)
      *initialText = requestInitial.c_str();
    if (extended)
      *extended = g_extended ? 1 : 0;
    g_state = OPEN;
    return true;
  }

  void HostTextDialog_complete(const char *text)
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state != OPEN)
      return;

    if (text)
    {
      g_result = text;
      g_state = SELECTED;
    }
    else
    {
      g_result.clear();
      g_state = CANCELLED;
    }
  }
}
