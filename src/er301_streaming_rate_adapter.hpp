#pragma once

#include "er301_audio_contract.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

// Causal host/ER-301 sample-rate bridge used by the automatic streaming input
// path. Incoming Rack samples are converted to the ER-301 clock without input
// batching. Every generated ER-301-rate sample is delivered immediately to
// the same per-sample engine callback used at native host rates.
//
// Output conversion is also causal. Linear interpolation is delayed by one
// ER-301 sample so both interpolation endpoints are already rendered.
//
// IMPORTANT: this is intentionally a low-latency linear SRC. It is not
// band-limited, so audio near Nyquist can alias when downsampling. The ER-301
// input timeline, including G1-G4 edge preservation, is the priority here.
// A production audio-quality SRC policy can replace the interpolation without
// changing the automatic streaming contract.
//
// No allocations, locks, I/O, or system calls occur in processSample().
namespace er301audio {

class StreamingRateAdapter
{
public:
  void configure(int rackSampleRate, int engineSampleRate, int frameLength)
  {
    mRackRate = std::max(8000, std::min(768000, rackSampleRate));
    mEngineRate = engineSampleRate == 96000 ? 96000 : 48000;
    mFrameLength = (frameLength >= 4 && frameLength <= MAX_FRAME &&
                    frameLength % 4 == 0) ? frameLength : 0;
    reset();
  }

  void reset()
  {
    mHostIndex = 0;
    mNextEngineInputIndex = 0;
    mRenderedOutputCount = 0;
    std::memset(mPreviousHostInput, 0, sizeof(mPreviousHostInput));
    std::memset(mGateRisePending, 0, sizeof(mGateRisePending));
    std::memset(mGatePeak, 0, sizeof(mGatePeak));
    std::memset(mOutputHistory, 0, sizeof(mOutputHistory));
  }

  int rackSampleRate() const { return mRackRate; }
  int engineSampleRate() const { return mEngineRate; }
  int frameLength() const { return mFrameLength; }

  template <typename RenderSample>
  void processSample(const float *rackInputs, float *rackOutputs,
                     RenderSample renderSample)
  {
    silence(rackOutputs);
    if (!rackInputs || !rackOutputs || !mFrameLength)
      return;

    float current[NUM_INPUTS];
    for (int ch = 0; ch < NUM_INPUTS; ++ch)
      current[ch] = finiteOrZero(rackInputs[ch]);

    if (mHostIndex == 0)
      std::memcpy(mPreviousHostInput, current, sizeof(current));

    // G1-G4 are dedicated gate jacks. Preserve a rising edge even when a
    // narrow Rack-rate pulse falls entirely between two ER-301 sample times.
    for (int ch = 0; ch < 4; ++ch)
    {
      if (current[ch] > 1.f && mPreviousHostInput[ch] <= 1.f)
      {
        mGateRisePending[ch] = true;
        mGatePeak[ch] = current[ch];
      }
      else if (mGateRisePending[ch] && current[ch] > mGatePeak[ch])
      {
        mGatePeak[ch] = current[ch];
      }
    }

    // Generate every ER-301 sample whose timestamp is now causally known.
    const uint64_t hostTimeInEngineTicks =
        mHostIndex * (uint64_t)mEngineRate;
    while (mNextEngineInputIndex * (uint64_t)mRackRate <=
           hostTimeInEngineTicks)
    {
      float alpha = 1.f;
      if (mHostIndex > 0)
      {
        const uint64_t sampleInEngineTicks =
            mNextEngineInputIndex * (uint64_t)mRackRate;
        const uint64_t previousHostTime =
            (mHostIndex - 1) * (uint64_t)mEngineRate;
        alpha = (float)(sampleInEngineTicks - previousHostTime) /
                (float)mEngineRate;
        alpha = std::max(0.f, std::min(1.f, alpha));
      }

      float engineInput[NUM_INPUTS];
      for (int ch = 0; ch < NUM_INPUTS; ++ch)
      {
        engineInput[ch] = mPreviousHostInput[ch] +
                          alpha * (current[ch] - mPreviousHostInput[ch]);
      }

      // Do not interpolate gates through threshold crossings. If a rising edge
      // occurred since the previous ER-301 sample, deliver its peak exactly
      // once; otherwise use the current gate level.
      for (int ch = 0; ch < 4; ++ch)
      {
        engineInput[ch] =
            mGateRisePending[ch] ? mGatePeak[ch] : current[ch];
        mGateRisePending[ch] = false;
        mGatePeak[ch] = 0.f;
      }

      float engineOutput[NUM_OUTPUTS] = {};
      renderSample(engineInput, engineOutput);

      OutputFrame &destination =
          mOutputHistory[mRenderedOutputCount % OUTPUT_HISTORY_FRAMES];
      for (int ch = 0; ch < NUM_OUTPUTS; ++ch)
        destination.samples[ch] = finiteOrZero(engineOutput[ch]);

      ++mRenderedOutputCount;
      ++mNextEngineInputIndex;
    }

    // Causal linear output interpolation with exactly one ER-301-sample
    // interpolation delay. Any larger latency visible here comes from the DSP
    // graph itself (for example the legacy one-frame compatibility suffix).
    const uint64_t hostTimeNumerator =
        mHostIndex * (uint64_t)mEngineRate;
    if (hostTimeNumerator >= (uint64_t)mRackRate)
    {
      const uint64_t targetNumerator =
          hostTimeNumerator - (uint64_t)mRackRate;
      const uint64_t firstIndex =
          targetNumerator / (uint64_t)mRackRate;
      const uint64_t secondIndex = firstIndex + 1;

      if (secondIndex < mRenderedOutputCount &&
          mRenderedOutputCount - firstIndex <= OUTPUT_HISTORY_FRAMES)
      {
        const float fraction =
            (float)(targetNumerator % (uint64_t)mRackRate) /
            (float)mRackRate;
        const OutputFrame &a =
            mOutputHistory[firstIndex % OUTPUT_HISTORY_FRAMES];
        const OutputFrame &b =
            mOutputHistory[secondIndex % OUTPUT_HISTORY_FRAMES];
        for (int ch = 0; ch < NUM_OUTPUTS; ++ch)
          rackOutputs[ch] = a.samples[ch] +
                            fraction * (b.samples[ch] - a.samples[ch]);
      }
    }

    std::memcpy(mPreviousHostInput, current, sizeof(current));
    ++mHostIndex;
  }

private:
  struct OutputFrame
  {
    float samples[NUM_OUTPUTS];
  };

  // 8192 samples comfortably covers the largest practical host/engine ratio
  // while keeping fixed, allocation-free storage.
  static constexpr int OUTPUT_HISTORY_FRAMES = 8192;

  static void silence(float *outputs)
  {
    if (!outputs)
      return;
    for (int ch = 0; ch < NUM_OUTPUTS; ++ch)
      outputs[ch] = 0.f;
  }

  float mPreviousHostInput[NUM_INPUTS] = {};
  bool mGateRisePending[4] = {};
  float mGatePeak[4] = {};
  OutputFrame mOutputHistory[OUTPUT_HISTORY_FRAMES] = {};

  int mRackRate = 48000;
  int mEngineRate = 48000;
  int mFrameLength = 0;
  uint64_t mHostIndex = 0;
  uint64_t mNextEngineInputIndex = 0;
  uint64_t mRenderedOutputCount = 0;
};

} // namespace er301audio
