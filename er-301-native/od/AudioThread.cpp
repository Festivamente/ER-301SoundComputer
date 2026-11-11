#include "AudioThread.h"
#include <od/tasks/ObjectCache.h>
#include <od/extras/BufferPool.h>
#include <od/tasks/TaskScheduler.h>
#include <od/tasks/ConnectionQueue.h>
#include <od/extras/LookupTables.h>
#include <od/extras/Profiler.h>
#include <od/config.h>
#include <hal/log.h>
#include <limits.h>

namespace od
{

  struct AudioThreadLocals
  {
    WatermarkedBufferPool<float> framePool;
    bool outOfFramesLatch = false;

    TaskScheduler tasks;
    InputTask *inputTask = 0;
    OutputTask *outputTask = 0;

    ConnectionQueue *conQ = 0;

    ObjectCache<FifoProbe> *fifoProbeCache = 0;

    ExecutionTimer audioTimer;
  };

  static AudioThreadLocals *local = 0;

  AudioThread::AudioThread()
  {
  }

  void AudioThread::init()
  {
    if (local)
    {
      logWarn("AudioThread::init called while already initialized.");
      return;
    }
    local = new AudioThreadLocals();
    local->framePool.allocate(globalConfig.frameLength, 1024);
    Outlet::initializeGlobalOutlets();
    LookupTables::initialize();
    Profiler::add(&local->audioTimer, "audio", true);

    // higher priority numbers are processed first
    local->inputTask = new InputTask();
    addTask(local->inputTask, INT_MAX - 1);

    local->outputTask = new OutputTask();
    addTask(local->outputTask, INT_MIN + 2);

    // probe cache
    local->fifoProbeCache = new ObjectCache<FifoProbe>("FifoProbeCache");
    addTask(local->fifoProbeCache, INT_MIN + 1);

    // connection queue is last
    local->conQ = new ConnectionQueue();
    addTask(local->conQ, INT_MIN);
  }

  void AudioThread::shutdown()
  {
    if (!local)
    {
      return;
    }

    const int scheduledTaskCount = local->tasks.size();
    if (scheduledTaskCount > 4)
    {
      logWarn("AudioThread::shutdown: %d tasks remain scheduled after Lua "
              "cleanup; applying defensive two-phase graph teardown.",
              scheduledTaskCount);
    }

    // The two process-global constant outlets obtain buffers from this
    // frame pool. Return them before destroying the pool so a subsequent
    // host initialization does not retain pointers into freed storage.
    if (ZeroOutput.mBuffer)
    {
      releaseFrame(ZeroOutput.mBuffer);
      ZeroOutput.mBuffer = 0;
    }
    if (OneOutput.mBuffer)
    {
      releaseFrame(OneOutput.mBuffer);
      OneOutput.mBuffer = 0;
    }

    delete local;
    local = 0;
  }

  bool AudioThread::isInitialized()
  {
    return local != 0;
  }

  void AudioThread::addTask(Task *task, int priority)
  {
    logAssert(task);
    task->mPriority = priority;
    local->tasks.add(task);
  }

  void AudioThread::removeTask(Task *task)
  {
    local->tasks.remove(task);
  }

  void AudioThread::beginTransaction()
  {
    local->tasks.beginTransaction();
  }

  void AudioThread::endTransaction()
  {
    local->tasks.endTransaction();
  }

  bool AudioThread::tryEnableStreaming()
  {
    return local && local->tasks.tryEnableRangeProcessing();
  }

  void AudioThread::disableStreaming()
  {
    if (local)
      local->tasks.setRangeProcessingEnabled(false);
  }

  bool AudioThread::streamingOutputIsImmediate()
  {
    return local && local->tasks.rangeProcessingIsImmediate();
  }

  bool AudioThread::processStreamingRange(float *inputs, float *outputs,
                                           int startSample, int sampleCount)
  {
    if (!local || !inputs || !outputs || startSample < 0 || sampleCount <= 0 ||
        sampleCount >= FRAMELENGTH ||
        startSample >= FRAMELENGTH ||
        sampleCount > FRAMELENGTH - startSample)
      return false;

    local->audioTimer.start();
    const bool success = local->tasks.tryProcessRange(
        inputs, outputs, startSample, sampleCount, FRAMELENGTH);
    local->audioTimer.stop();
    return success;
  }

  InputTask *AudioThread::getInputTask()
  {
    return local->inputTask;
  }

  OutputTask *AudioThread::getOutputTask()
  {
    return local->outputTask;
  }

  FifoProbe *AudioThread::getFifoProbe()
  {
    return local->fifoProbeCache->get();
  }

  void AudioThread::releaseFifoProbe(FifoProbe *probe)
  {
    local->fifoProbeCache->release(probe);
  }

  float *AudioThread::getFrame()
  {
    float *frame = local->framePool.get();
    if (frame)
    {
      local->outOfFramesLatch = false;
      return frame;
    }
    else
    {
      if (!local->outOfFramesLatch)
      {
        local->outOfFramesLatch = true;
        if (local->framePool.mPoolSizeInBuffers == 0)
        {
          logFatal("AudioThread::getFrame(): pool not allocated yet!");
        }
        else
        {
          logFatal("AudioThread::getFrame(): out of buffers!");
        }
      }
      return 0;
    }
  }

  void AudioThread::releaseFrame(float *frame)
  {
    local->framePool.release(frame);
  }

  void AudioThread::connect(Outlet *outlet, Inlet *inlet, Object *object)
  {
    local->conQ->pushConnection(outlet, inlet, object);
  }

  void AudioThread::disconnect(Inlet *inlet, Object *object)
  {
    local->conQ->pushDisconnection(inlet, object);
  }

  void AudioThread::disconnect(Outlet *outlet, Object *object)
  {
    local->conQ->pushDisconnection(outlet, object);
  }

  void AudioThread::printStatus()
  {
    logInfo("FramePool: %d of %d (watermark=%d)",
            local->framePool.countBuffersInUse(),
            local->framePool.mPoolSizeInBuffers,
            local->framePool.mMaxBuffersUsed);
    logInfo("Fifo Probes: %lu active %lu free",
            local->fifoProbeCache->activeSize(),
            local->fifoProbeCache->freeSize());
    local->conQ->printState();
  }

} /* namespace od */

extern "C"
{
  void Pump_callback(float *inputs, float *outputs)
  {
    if (!od::local)
    {
      return;
    }
    od::local->audioTimer.start();
    od::local->tasks.process(inputs, outputs);
    od::local->audioTimer.stop();
  }
}

// Independent experimental entry points. Rack does not call these in 2F.
// A partial-frame failure is terminal for that frame: the host must NOT
// retry the same frame via Pump_callback(), which would process samples twice.
extern "C" bool Pump_try_enable_streaming(void)
{
  return od::AudioThread::tryEnableStreaming();
}

extern "C" void Pump_disable_streaming(void)
{
  od::AudioThread::disableStreaming();
}

extern "C" bool Pump_streaming_output_is_immediate(void)
{
  return od::AudioThread::streamingOutputIsImmediate();
}

extern "C" bool Pump_callback_range(float *inputs, float *outputs,
                                      int startSample, int sampleCount)
{
  return od::AudioThread::processStreamingRange(inputs, outputs,
                                                  startSample, sampleCount);
}
