#pragma once

#include <od/tasks/InputTask.h>
#include <od/tasks/OutputTask.h>
#include <od/objects/measurement/FifoProbe.h>

namespace od
{

  class AudioThread
  {
  public:
    static void init();
#ifndef SWIGLUA
    static void shutdown();
    static bool isInitialized();
#endif

    static void addTask(Task *task, int priority);
    static void removeTask(Task *task);
    static void beginTransaction();
    static void endTransaction();

#ifndef SWIGLUA
    // Phase 2F: prepared for an opt-in streaming host callback. These do
    // not activate streaming from the existing Pump_callback() entry point.
    static bool tryEnableStreaming();
    static void disableStreaming();
    static bool streamingOutputIsImmediate();
    static bool processStreamingRange(float *inputs, float *outputs,
                                      int startSample, int sampleCount);
#endif

    static InputTask *getInputTask();
    static OutputTask *getOutputTask();

    static void connect(Outlet *outlet, Inlet *inlet, Object *object = 0);
    static void disconnect(Inlet *inlet, Object *object = 0);
    static void disconnect(Outlet *outlet, Object *object = 0);

    static void printStatus();

#ifndef SWIGLUA
    static FifoProbe *getFifoProbe();
    static void releaseFifoProbe(FifoProbe *probe);

    // Returns (i.e. allocates) a FRAMELENGTH buffer from the pre-allocated pool of frame buffers.
    static float *getFrame();
    // Releases (i.e. frees) the previously gotten frame buffer.
    static void releaseFrame(float *frame);
    // The previous 2 calls are constant-time non-locking and thus safe to call in the audio thread.

#endif

  private:
    AudioThread();
  };

} /* namespace od */

#ifndef SWIGLUA
// Experimental C ABI for the future VCV streaming bridge. All calls are
// default-off; Pump_callback() remains the untouched legacy path.
extern "C" bool Pump_try_enable_streaming(void);
extern "C" void Pump_disable_streaming(void);
extern "C" bool Pump_streaming_output_is_immediate(void);
extern "C" bool Pump_callback_range(float *inputs, float *outputs,
                                      int startSample, int sampleCount);
#endif
