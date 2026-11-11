#include <od/tasks/TaskScheduler.h>
#include <od/tasks/Task.h>
#include <algorithm>

namespace od
{

  TaskScheduler::TaskScheduler()
  {
  }

  TaskScheduler::~TaskScheduler()
  {
    // Detach the scheduler's list first so any destructor-side removeTask()
    // calls cannot mutate the container being traversed. The scheduler's
    // references keep every task alive through the prepare pass.
    std::vector<Task *> tasks;
    mMutex.enter();
    tasks.swap(mTasks);
    mTransactions.clear();
    mTransactionDepth = 0;
    for (Task *task : tasks)
    {
      task->mActive = false;
#if TASK_TIMING_ENABLED
      Profiler::remove(&task->mExecutionTimer);
#endif
    }
    mMutex.leave();

    // First sever cross-task and graph connections while all tasks are alive.
    for (Task *task : tasks)
    {
      task->prepareForShutdown();
    }

    // Only after the graph is disconnected may scheduler ownership be
    // released. This avoids re-entrant UnitChain destruction from an Inlet or
    // Outlet destructor midway through another task's teardown.
    for (Task *task : tasks)
    {
      task->release();
    }
  }

  void TaskScheduler::process(float *inputs, float *outputs)
  {
    mMutex.enter();
    for (Task *task : mTasks)
    {
#if TASK_TIMING_ENABLED
      task->mExecutionTimer.start();
#endif
      task->process(inputs, outputs);
#if TASK_TIMING_ENABLED
      task->mExecutionTimer.stop();
#endif
    }
    mMutex.leave();
  }

  void TaskScheduler::setRangeProcessingEnabled(bool enabled)
  {
    mMutex.enter();
    mRangeProcessingEnabled = enabled;
    // A mode switch must not resume an already partly processed frame.
    mNextRangeSample = 0;
    mRangeFrameLength = 0;
    mRangePrefixCount = 0;
    mRangeImmediate = false;
    mMutex.leave();
  }

  // Streaming is always allowed when the leading part of the schedule can
  // consume ranges. The common case is InputTask at the head of the schedule.
  //
  // If every audio-producing task through OutputTask supports ranges, output
  // is immediate. Otherwise the supported prefix still receives each input
  // sample on time and the remaining legacy tasks run once, in order, after
  // the final sample of the frame. This keeps old/third-party units compatible
  // without allowing one legacy unit to disable low-latency input capture.
  bool TaskScheduler::analyzeRangeScheduleWithoutLock(int &prefixCount,
                                                       bool &immediate) const
  {
    prefixCount = 0;
    immediate = false;

    if (mTasks.empty() || mTransactionDepth != 0 || !mTransactions.empty())
      return false;

    bool stoppedOnFrameEnd = false;
    for (Task *task : mTasks)
    {
      if (task->supportsRange())
      {
        ++prefixCount;
        continue;
      }

      stoppedOnFrameEnd = task->supportsFrameEnd();
      break;
    }

    // InputTask is range-capable and is scheduled first. Refuse streaming only
    // if even the schedule head cannot consume ranges.
    if (prefixCount == 0)
      return false;

    // Reaching the frame-end-only suffix means all ordinary DSP and OutputTask
    // before it are streaming-capable. If there is no suffix at all, the whole
    // schedule is range-capable.
    immediate = prefixCount == (int)mTasks.size() || stoppedOnFrameEnd;
    return true;
  }

  bool TaskScheduler::tryEnableRangeProcessing()
  {
    mMutex.enter();
    int prefixCount = 0;
    bool immediate = false;
    // Only allow a mode transition on a fresh frame; never discard a partly
    // processed frame in order to advertise a successful opt-in.
    const bool ready = mNextRangeSample == 0 &&
                       analyzeRangeScheduleWithoutLock(prefixCount, immediate);
    if (ready)
    {
      mRangeProcessingEnabled = true;
      mRangeFrameLength = 0;
      mRangePrefixCount = prefixCount;
      mRangeImmediate = immediate;
    }
    mMutex.leave();
    return ready;
  }

  bool TaskScheduler::rangeProcessingIsImmediate()
  {
    mMutex.enter();
    const bool result = mRangeProcessingEnabled && mRangeImmediate;
    mMutex.leave();
    return result;
  }

  bool TaskScheduler::tryProcessRange(float *inputs, float *outputs,
                                       int startSample, int sampleCount,
                                       int frameLength)
  {
    // Full frames belong to the existing process() function. Never silently
    // fall back to block processing when a partial-range request fails.
    if (frameLength <= 0 || startSample < 0 || sampleCount <= 0 ||
        startSample >= frameLength || sampleCount >= frameLength ||
        sampleCount > frameLength - startSample)
      return false;

    mMutex.enter();
    if (!mRangeProcessingEnabled || mTasks.empty() || mTransactionDepth != 0 ||
        !mTransactions.empty() || startSample != mNextRangeSample ||
        (mNextRangeSample != 0 && frameLength != mRangeFrameLength))
    {
      mMutex.leave();
      return false;
    }

    // Re-check the schedule every slice. Graph edits already invalidate range
    // mode in add/remove, but this also catches capability changes inside a
    // task. A changed boundary must wait for a fresh frame.
    int prefixCount = 0;
    bool immediate = false;
    if (!analyzeRangeScheduleWithoutLock(prefixCount, immediate) ||
        prefixCount != mRangePrefixCount ||
        immediate != mRangeImmediate)
    {
      mRangeProcessingEnabled = false;
      mNextRangeSample = 0;
      mRangeFrameLength = 0;
      mRangePrefixCount = 0;
      mRangeImmediate = false;
      mMutex.leave();
      return false;
    }

    bool success = true;

    // Range-process the supported prefix. With a legacy graph this still
    // includes InputTask, so every external input lands at its exact sample
    // position even though downstream legacy units remain frame based.
    for (int i = 0; i < mRangePrefixCount; ++i)
    {
      Task *task = mTasks[i];
      if (!task->processRange(inputs, outputs, startSample, sampleCount))
      {
        success = false;
        break;
      }
    }

    const bool lastSlice = startSample + sampleCount == frameLength;
    if (success && lastSlice)
    {
      // Everything after the range-capable prefix runs once per frame in its
      // original scheduler order. For a fully streaming graph this is only
      // the frame-end maintenance suffix. For a legacy graph it includes the
      // unsupported unit chain(s), OutputTask, and maintenance tasks.
      for (int i = mRangePrefixCount; i < (int)mTasks.size(); ++i)
      {
#if TASK_TIMING_ENABLED
        mTasks[i]->mExecutionTimer.start();
#endif
        mTasks[i]->process(inputs, outputs);
#if TASK_TIMING_ENABLED
        mTasks[i]->mExecutionTimer.stop();
#endif
      }
    }

    if (success)
    {
      mNextRangeSample = startSample + sampleCount;
      mRangeFrameLength = frameLength;
      if (mNextRangeSample == frameLength)
      {
        mNextRangeSample = 0;
        mRangeFrameLength = 0;
      }
    }
    else
    {
      // A task that advertised range support failed its contract. Fail closed;
      // the host recovery path will silence the partial frame and retry after
      // one complete legacy frame.
      mRangeProcessingEnabled = false;
      mNextRangeSample = 0;
      mRangeFrameLength = 0;
      mRangePrefixCount = 0;
      mRangeImmediate = false;
    }
    mMutex.leave();
    return success;
  }

  void TaskScheduler::add(Task *task)
  {
    mMutex.enter();
    // Graph edits invalidate an in-progress partial processing schedule.
    mRangeProcessingEnabled = false;
    mNextRangeSample = 0;
    mRangeFrameLength = 0;
    mRangePrefixCount = 0;
    mRangeImmediate = false;
    if (mTransactionDepth == 0)
    {
      if (!task->mActive)
      {
        task->attach();
#if TASK_TIMING_ENABLED
        Profiler::add(&task->mExecutionTimer, task->mName.c_str(), false);
#endif
        // keep priorities sorted in decreasing order
        auto it = std::upper_bound(
            mTasks.begin(), mTasks.end(), task,
            [](Task *const lhs, Task *const rhs) { return lhs->mPriority > rhs->mPriority; });
        mTasks.insert(it, task);
        task->mActive = true;
      }
    }
    else
    {
      mTransactions.emplace_back(1, task);
    }
    mMutex.leave();
  }

  void TaskScheduler::remove(Task *task)
  {
    mMutex.enter();
    mRangeProcessingEnabled = false;
    mNextRangeSample = 0;
    mRangeFrameLength = 0;
    mRangePrefixCount = 0;
    mRangeImmediate = false;
    if (mTransactionDepth == 0)
    {
      auto i = std::find(mTasks.begin(), mTasks.end(), task);
      if (i != mTasks.end())
      {
        mTasks.erase(i);
#if TASK_TIMING_ENABLED
        Profiler::remove(&task->mExecutionTimer);
#endif
        task->mActive = false;
        task->release();
      }
    }
    else
    {
      mTransactions.emplace_back(0, task);
    }
    mMutex.leave();
  }

  void TaskScheduler::beginTransaction()
  {
    mTransactionDepth++;
  }

  void TaskScheduler::endTransaction()
  {
    if (mTransactionDepth > 0)
    {
      mTransactionDepth--;
      if (mTransactionDepth == 0)
      {
        for (std::pair<int, Task *> &trans : mTransactions)
        {
          if (trans.first == 0)
          {
            remove(trans.second);
          }
          else
          {
            add(trans.second);
          }
        }
        mTransactions.clear();
      }
    }
  }

  int TaskScheduler::size()
  {
    mMutex.enter();
    int result = (int)mTasks.size();
    mMutex.leave();
    return result;
  }

} /* namespace od */
