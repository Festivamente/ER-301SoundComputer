#include <emu/tls.h>
#include <od/extras/ReferenceCounted.h>
#include <hal/concurrency/Thread.h>
//#define BUILDOPT_DEBUG_LEVEL 10
#include <hal/log.h>
#include <SDL2/SDL_thread.h>
#include <SDL2/SDL_timer.h>

#ifdef BUILDOPT_VERBOSE
#include <od/extras/Utils.h>
#include <typeinfo>
#endif

namespace od
{

  struct ThreadRunner
  {
    static int threadEntry(void *ptr)
    {
      od::Thread *thread = (od::Thread *)ptr;
      logAssert(thread);
      TLS_setName(thread->mName.c_str());
      logInfo("Thread starting.");
      if (thread->mPriority < TASK_PRIORITY_REALTIME)
      {
        SDL_SetThreadPriority(SDL_THREAD_PRIORITY_NORMAL);
      }
      else
      {
        SDL_SetThreadPriority(SDL_THREAD_PRIORITY_NORMAL);
        // SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
      }
      thread->mThreadRunning = true;
      thread->run();
      thread->mThreadRunning = false;
      return 0;
    }
  };

  void Thread::sleep(uint32_t timeout)
  {
    SDL_Delay(timeout);
  }

  void Thread::yield()
  {
    SDL_Delay(0);
  }

  Thread::Thread(const char *name) : mName(name)
  {
    logDebug(1, "Thread(%s): constructor", name);
  }

  Thread::Thread(const char *name, int priority) : mName(name), mPriority(priority)
  {
    logDebug(1, "Thread(%s): constructor", name);
  }

  Thread::~Thread()
  {
    logDebug(1, "Thread(%s): destructor", mName.c_str());
    if (mThreadHandle)
    {
      stop();
      join();
    }
  }

  void Thread::start()
  {
    if (mThreadHandle)
    {
      logWarn("Thread(%s): start requested while already started.",
              mName.c_str());
      return;
    }
    mEvents.clear(onThreadQuit);
#ifdef BUILDOPT_VERBOSE
    std::string classname = demangle(typeid(*this).name());
    logDebug(1, "%s(0x%x): start", classname.c_str(), this);
#endif
    mThreadHandle = (void *)SDL_CreateThread(ThreadRunner::threadEntry, mName.c_str(), (void *)this);
    if (mThreadHandle == 0)
    {
      logError("Failed to create SDL Thread.");
    }
  }

  bool Thread::running()
  {
    return mThreadRunning.load();
  }

  void Thread::stop()
  {
    if (!mThreadHandle)
    {
      return;
    }
#ifdef BUILDOPT_VERBOSE
    std::string classname = demangle(typeid(*this).name());
    logDebug(1, "%s(0x%x): requesting stop", classname.c_str(), this);
#endif
    mEvents.post(onThreadQuit);
  }

  void Thread::join()
  {
    if (mThreadHandle)
    {
#ifdef BUILDOPT_VERBOSE
      std::string classname = demangle(typeid(*this).name());
#endif
      int ret;
      SDL_Thread *thread = (SDL_Thread *)mThreadHandle;
      SDL_WaitThread(thread, &ret);
      mThreadHandle = 0;
      mThreadRunning.store(false);
#ifdef BUILDOPT_VERBOSE
      logDebug(1, "%s(0x%x): stopped", classname.c_str(), this);
#endif
    }
  }

} /* namespace od */
