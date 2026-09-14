#include <hal/gpio.h>
#include <hal/constants.h>
#include <hal/fifo.h>
#include <hal/events.h>
#include <hal/encoder.h>
#include <hal/concurrency/EventFlags.h>
#include <string.h>
#ifdef ER301_VCV_HOST
#include <chrono>
#include <condition_variable>
#include <mutex>
#endif
//#define BUILDOPT_VERBOSE
//#define BUILDOPT_DEBUG_LEVEL 10
#include <hal/log.h>

#define REPEAT_PERIOD 3
#define REPEAT_DELAY 25

typedef struct
{
  fifo_t Q;
  od::EventFlags events;
#define onPost od::EventFlags::flag00
  int buttonTimer[19] = {0};
  int lastEncoderValue = 0;
} Local;

static Local local;

#ifdef ER301_VCV_HOST
namespace
{
  struct HostSaveState
  {
    std::mutex mutex;
    std::condition_variable condition;
    bool active = false;
    bool done = false;
    bool result = false;
    bool abandoned = false;
  };

  HostSaveState hostSave;
}
#endif

static void checkButtonRepeat(uint32_t id)
{
  int i = id - BUTTON_MAIN1;
  if (Button_pressed(id))
  {
    if (local.buttonTimer[i] > REPEAT_PERIOD + REPEAT_DELAY)
    {
      local.buttonTimer[i] = REPEAT_DELAY;
      Events_push(EVENT(EVENT_REPEAT, id));
    }
    else
    {
      local.buttonTimer[i]++;
    }
  }
  else
  {
    local.buttonTimer[i] = 0;
  }
}

static void checkEncoder()
{
  int value = Encoder_getValue();
  if (value != local.lastEncoderValue)
  {
    Events_push(EVENT_KNOB);
    local.lastEncoderValue = value;
  }
}

static void configureButtonEvents(uint32_t id)
{
  Gpio_setEvents(id, EVENT(EVENT_RELEASE, id), EVENT(EVENT_PRESS, id));
}

//////////////////

extern "C"
{

  void Events_init(void)
  {
    HostState_cancel();
    fifo_init(&local.Q);
    local.events.clear(onPost);
    memset(local.buttonTimer, 0, sizeof(local.buttonTimer));

    configureButtonEvents(BUTTON_MAIN1);
    configureButtonEvents(BUTTON_MAIN2);
    configureButtonEvents(BUTTON_MAIN3);
    configureButtonEvents(BUTTON_MAIN4);
    configureButtonEvents(BUTTON_MAIN5);
    configureButtonEvents(BUTTON_MAIN6);
    configureButtonEvents(BUTTON_SUB1);
    configureButtonEvents(BUTTON_SUB2);
    configureButtonEvents(BUTTON_SUB3);
    configureButtonEvents(BUTTON_ENTER);
    configureButtonEvents(BUTTON_UP);
    configureButtonEvents(BUTTON_SHIFT);
    configureButtonEvents(BUTTON_DIAL1);
    configureButtonEvents(BUTTON_DIAL2);
    configureButtonEvents(BUTTON_DIAL3);
    configureButtonEvents(BUTTON_SELECT1);
    configureButtonEvents(BUTTON_SELECT2);
    configureButtonEvents(BUTTON_SELECT3);
    configureButtonEvents(BUTTON_SELECT4);
    Gpio_setEvents(TOGGLE_MODE_A, EVENT_MODE, EVENT_MODE);
    Gpio_setEvents(TOGGLE_MODE_B, EVENT_MODE, EVENT_MODE);
    Gpio_setEvents(TOGGLE_STORAGE_A, EVENT_STORAGE, EVENT_STORAGE);
    Gpio_setEvents(TOGGLE_STORAGE_B, EVENT_STORAGE, EVENT_STORAGE);

    local.lastEncoderValue = Encoder_getValue();
  }

  void Events_push(uint32_t e)
  {
    logDebug(1,"push type=%d id=%d", EVENT_TYPE(e), EVENT_ID(e));
    fifo_push(&local.Q, e);
    local.events.post(onPost);
  }

  void Events_clear(void)
  {
    HostState_cancel();
    fifo_init(&local.Q);
    local.events.clear(onPost);
    memset(local.buttonTimer, 0, sizeof(local.buttonTimer));
  }

  static void check()
  {
    checkButtonRepeat(BUTTON_MAIN1);
    checkButtonRepeat(BUTTON_MAIN2);
    checkButtonRepeat(BUTTON_MAIN3);
    checkButtonRepeat(BUTTON_MAIN4);
    checkButtonRepeat(BUTTON_MAIN5);
    checkButtonRepeat(BUTTON_MAIN6);
    checkButtonRepeat(BUTTON_SUB1);
    checkButtonRepeat(BUTTON_SUB2);
    checkButtonRepeat(BUTTON_SUB3);
    checkButtonRepeat(BUTTON_ENTER);
    checkButtonRepeat(BUTTON_UP);
    checkButtonRepeat(BUTTON_SHIFT);
    checkButtonRepeat(BUTTON_DIAL1);
    checkButtonRepeat(BUTTON_DIAL2);
    checkButtonRepeat(BUTTON_DIAL3);
    checkButtonRepeat(BUTTON_SELECT1);
    checkButtonRepeat(BUTTON_SELECT2);
    checkButtonRepeat(BUTTON_SELECT3);
    checkButtonRepeat(BUTTON_SELECT4);
    checkEncoder();
  }

  bool Events_waitWithTimeout(uint32_t timeout)
  {
    check();
    return local.events.waitForAll(onPost, timeout) & onPost;
  }

  void Events_wait(void)
  {
    check();
    local.events.waitForAll(onPost);
  }

  uint32_t Events_pull(void)
  {
    uint32_t value = EVENT_NONE;
    fifo_pop(&local.Q, &value);
    return value;
  }

#ifdef ER301_VCV_HOST
  bool HostState_requestSave(uint32_t timeoutMs)
  {
    std::unique_lock<std::mutex> lock(hostSave.mutex);
    if (hostSave.active)
    {
      logWarn("HostState_requestSave: another request is already active.");
      return false;
    }

    hostSave.active = true;
    hostSave.done = false;
    hostSave.result = false;
    hostSave.abandoned = false;
    lock.unlock();

    Events_push(EVENT_HOST_SAVE);

    lock.lock();
    const bool completed = hostSave.condition.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), []() {
          return hostSave.done || !hostSave.active;
        });
    if (!completed)
    {
      // The Lua thread may already be serializing. Leave the request active
      // until it calls HostState_finishSave(), but do not block Rack forever.
      hostSave.abandoned = true;
      logWarn("HostState_requestSave: timed out after %u ms.", timeoutMs);
      return false;
    }

    const bool result = hostSave.done && hostSave.result;
    hostSave.active = false;
    hostSave.done = false;
    hostSave.abandoned = false;
    return result;
  }

  void HostState_finishSave(bool success)
  {
    std::lock_guard<std::mutex> lock(hostSave.mutex);
    if (!hostSave.active)
    {
      return;
    }
    hostSave.result = success;
    hostSave.done = true;
    if (hostSave.abandoned)
    {
      hostSave.active = false;
    }
    hostSave.condition.notify_all();
  }

  void HostState_cancel(void)
  {
    std::lock_guard<std::mutex> lock(hostSave.mutex);
    hostSave.active = false;
    hostSave.done = true;
    hostSave.result = false;
    hostSave.abandoned = false;
    hostSave.condition.notify_all();
  }
#else
  bool HostState_requestSave(uint32_t timeoutMs)
  {
    (void)timeoutMs;
    return false;
  }

  void HostState_finishSave(bool success)
  {
    (void)success;
  }

  void HostState_cancel(void)
  {
  }
#endif
}
