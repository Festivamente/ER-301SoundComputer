#include <od/tasks/InputTask.h>
#include <od/config.h>
#include <od/tasks/StreamingFrameIO.h>
#include <hal/channels.h>
#include <string.h>

namespace od
{

  InputTask::InputTask() : Task("InputTask")
  {

    own(mA1);
    own(mA2);
    own(mA3);

    own(mB1);
    own(mB2);
    own(mB3);

    own(mC1);
    own(mC2);
    own(mC3);

    own(mD1);
    own(mD2);
    own(mD3);

    own(mG1);
    own(mG2);
    own(mG3);
    own(mG4);

    own(mIN1);
    own(mIN2);
    own(mIN3);
    own(mIN4);

    // and don't let go
  }

  InputTask::~InputTask()
  {
  }

  static inline void copyFromInputs(float *__restrict__ dst,
                                    float *__restrict__ src, uint32_t channel)
  {
    float *end = dst + FRAMELENGTH;
    src += channel;
    for (; dst < end; src += NUM_INPUT_CHANNELS, dst++)
    {
      *dst = *src;
    }
  }

#define INPUTCOPY(x)                                  \
  if (m##x.isConnected())                             \
  {                                                   \
    copyFromInputs(m##x.buffer(), inputs, INPUT_##x); \
  }

  void InputTask::process(float *inputs, float *outputs)
  {
    // cache the last value of input frame for display purposes
    memcpy(mLastInput,
           &inputs[(NUM_INPUT_CHANNELS) * (FRAMELENGTH - 1)],
           NUM_INPUT_CHANNELS * sizeof(float));

    INPUTCOPY(A1);
    INPUTCOPY(A2);
    INPUTCOPY(A3);

    INPUTCOPY(B1);
    INPUTCOPY(B2);
    INPUTCOPY(B3);

    INPUTCOPY(C1);
    INPUTCOPY(C2);
    INPUTCOPY(C3);

    INPUTCOPY(D1);
    INPUTCOPY(D2);
    INPUTCOPY(D3);

    INPUTCOPY(G1);
    INPUTCOPY(G2);
    INPUTCOPY(G3);
    INPUTCOPY(G4);

    INPUTCOPY(IN1);
    INPUTCOPY(IN2);
    INPUTCOPY(IN3);
    INPUTCOPY(IN4);
  }

  bool InputTask::processRange(float *inputs, float *outputs,
                               int startSample, int sampleCount)
  {
    (void)outputs;
    if (!inputs || !streaming::validFrameRange(startSample, sampleCount, FRAMELENGTH))
      return false;

    // Match the legacy display snapshot: update only at the end of a frame.
    if (startSample + sampleCount == FRAMELENGTH)
    {
      memcpy(mLastInput,
             &inputs[NUM_INPUT_CHANNELS * (FRAMELENGTH - 1)],
             NUM_INPUT_CHANNELS * sizeof(float));
    }

#define INPUTCOPY_SLICE(x) \
    if (m##x.isConnected()) \
      streaming::copyInputChannel(m##x.buffer(), inputs, INPUT_##x, \
                                  startSample, sampleCount);
    INPUTCOPY_SLICE(A1);
    INPUTCOPY_SLICE(A2);
    INPUTCOPY_SLICE(A3);
    INPUTCOPY_SLICE(B1);
    INPUTCOPY_SLICE(B2);
    INPUTCOPY_SLICE(B3);
    INPUTCOPY_SLICE(C1);
    INPUTCOPY_SLICE(C2);
    INPUTCOPY_SLICE(C3);
    INPUTCOPY_SLICE(D1);
    INPUTCOPY_SLICE(D2);
    INPUTCOPY_SLICE(D3);
    INPUTCOPY_SLICE(G1);
    INPUTCOPY_SLICE(G2);
    INPUTCOPY_SLICE(G3);
    INPUTCOPY_SLICE(G4);
    INPUTCOPY_SLICE(IN1);
    INPUTCOPY_SLICE(IN2);
    INPUTCOPY_SLICE(IN3);
    INPUTCOPY_SLICE(IN4);
#undef INPUTCOPY_SLICE
    return true;
  }

  void InputTask::prepareForShutdown()
  {
    // Disconnect every downstream inlet while all scheduled tasks are still
    // retained by TaskScheduler. This prevents outlet destruction from
    // recursively releasing partially torn-down UnitChains.
    mA1.disconnect();
    mA2.disconnect();
    mA3.disconnect();
    mB1.disconnect();
    mB2.disconnect();
    mB3.disconnect();
    mC1.disconnect();
    mC2.disconnect();
    mC3.disconnect();
    mD1.disconnect();
    mD2.disconnect();
    mD3.disconnect();
    mG1.disconnect();
    mG2.disconnect();
    mG3.disconnect();
    mG4.disconnect();
    mIN1.disconnect();
    mIN2.disconnect();
    mIN3.disconnect();
    mIN4.disconnect();
  }

} /* namespace od */
