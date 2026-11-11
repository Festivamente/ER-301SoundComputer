#pragma once

#include "er301_audio_contract.hpp"
#include <cstring>

// Automatic host-side streaming state machine. Rack drives this at the
// ER-301 engine sample rate, directly at 48/96 kHz or through the causal
// sample-rate bridge at other host rates. All callbacks operate on the real
// ER-301 engine. No allocations, file I/O or logging occur here.
//
// The host chooses a processing mode at frame boundaries. A failed slice is
// terminal for the CURRENT frame: silence its remainder and NEVER replay those
// samples with the legacy callback. After one full legacy frame, the scheduler
// may opt back in following a fresh eligibility preflight. Repeated runtime
// failures are latched to prevent an endless glitch loop.
namespace er301audio {

class NativeStreamingPilot
{
public:
  void reset(int frameLength = 0)
  {
    mFrameLength = valid(frameLength) ? frameLength : 0;
    mIndex = 0;
    mStreaming = false;
    mFailed = false;
    mDropThisFrame = false;
    mRecoveryFramesRemaining = 0;
    mConsecutiveRuntimeFailures = 0;
    std::memset(mInputs, 0, sizeof(mInputs));
    std::memset(mLegacyOutputs, 0, sizeof(mLegacyOutputs));
  }

  int index() const { return mIndex; }
  bool active() const { return mStreaming && !mFailed; }
  bool failed() const { return mFailed; }
  bool recovering() const { return mDropThisFrame || mRecoveryFramesRemaining > 0; }
  int frameLength() const { return mFrameLength; }

  template<class TryEnable, class Disable, class ProcessSample, class ProcessFrame>
  void processSample(const float* input, float* output, int frameLength,
                     bool pilotRequested,
                     TryEnable tryEnable, Disable disable,
                     ProcessSample streamSample, ProcessFrame legacyFrame)
  {
    if (!output) return;
    std::memset(output, 0, NUM_OUTPUTS * sizeof(float));
    if (!input || !valid(frameLength)) return;
    if (frameLength != mFrameLength) reset(frameLength);

    // Switch modes only at a NEW frame. Never replay samples already handled
    // by the partial-range scheduler using the full-frame callback.
    if (mIndex == 0)
    {
      if (!pilotRequested && mStreaming)
      {
        disable();
        mStreaming = false;
        std::memset(mLegacyOutputs, 0, sizeof(mLegacyOutputs));
      }
      else if (pilotRequested && !mStreaming && !mFailed &&
               mRecoveryFramesRemaining == 0)
      {
        mStreaming = tryEnable();
        if (mStreaming)
          std::memset(mLegacyOutputs, 0, sizeof(mLegacyOutputs));
      }
    }

    const int offset = mIndex;
    std::memcpy(mInputs + offset * NUM_INPUTS,
                input, NUM_INPUTS * sizeof(float));

    if (mDropThisFrame)
    {
      // Silence remainder of an invalidated streaming frame. No more DSP
      // callbacks until the next frame begins in legacy mode.
    }
    else if (mStreaming)
    {
      if (!streamSample(input, output, offset))
      {
        // Graph changes invalidate range scheduling. A runtime error looks
        // identical to that invalidation here, so avoid replaying any part
        // of this frame and preflight again after a full legacy frame.
        // Latch only after repeated actual mid-stream failures.
        std::memset(output, 0, NUM_OUTPUTS * sizeof(float));
        ++mConsecutiveRuntimeFailures;
        mFailed = mConsecutiveRuntimeFailures >= MAX_RUNTIME_FAILURES;
        mRecoveryFramesRemaining = mFailed ? 0 : 1;
        mDropThisFrame = true;
        mStreaming = false;
        disable();
      }
    }
    else
    {
      std::memcpy(output,
                  mLegacyOutputs + offset * NUM_OUTPUTS,
                  NUM_OUTPUTS * sizeof(float));
    }

    ++mIndex;
    if (mIndex == mFrameLength)
    {
      // Only a *fully legacy* frame may enter the legacy DSP. On failed
      // streaming frames, wait for the next full frame to restore cadence.
      if (!mStreaming && !mDropThisFrame)
      {
        legacyFrame(mInputs, mLegacyOutputs);
        if (mRecoveryFramesRemaining > 0)
          --mRecoveryFramesRemaining;
      }
      else if (mStreaming)
      {
        // A completely successful streamed frame clears the failure streak.
        mConsecutiveRuntimeFailures = 0;
      }
      if (mDropThisFrame)
      {
        // Preserve output silence until a NEW legacy frame is rendered.
        std::memset(mLegacyOutputs, 0, sizeof(mLegacyOutputs));
      }
      mIndex = 0;
      mDropThisFrame = false;
    }
  }

private:
  static constexpr int MAX_RUNTIME_FAILURES = 3;

  static bool valid(int frameLength)
  {
    return frameLength >= 4 && frameLength <= MAX_FRAME && frameLength % 4 == 0;
  }

  float mInputs[MAX_FRAME * NUM_INPUTS] = {};
  float mLegacyOutputs[MAX_FRAME * NUM_OUTPUTS] = {};
  int mFrameLength = 0;
  int mIndex = 0;
  bool mStreaming = false;
  bool mFailed = false;
  bool mDropThisFrame = false;
  int mRecoveryFramesRemaining = 0;
  int mConsecutiveRuntimeFailures = 0;
};

} // namespace er301audio
