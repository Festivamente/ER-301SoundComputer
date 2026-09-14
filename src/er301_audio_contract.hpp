#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

// Small, host-independent pieces of the ER-301 Sound Computer audio contract.
//
// Keeping these operations separate from Rack and the ER-301 runtime makes
// the channel order, voltage scaling, limiter, and block latency directly
// testable without booting Lua or opening an audio device.
namespace er301audio {

static constexpr int NUM_INPUTS = 20;
static constexpr int NUM_OUTPUTS = 4;
static constexpr int MAX_FRAME = 128;
static constexpr float UNITS_PER_VOLT = 0.1f;
static constexpr float VOLTS_PER_UNIT = 10.0f;

inline int chooseNativeSampleRate(int rackSampleRate)
{
  // The ER-301 has authentic 48 kHz and 96 kHz firmware modes. Choose the
  // nearer mode; 72 kHz is their midpoint.
  return rackSampleRate >= 72000 ? 96000 : 48000;
}

// Rack panel-jack order:
// G1 G2 G3 G4, IN1 IN2 IN3 IN4,
// A1 A2 A3, B1 B2 B3, C1 C2 C3, D1 D2 D3.
//
// Engine frame order is the physical ADC/modulator channel order defined by
// hal/channels.h. Every value 0..19 appears exactly once.
static constexpr int JACK_TO_ENGINE_CHANNEL[NUM_INPUTS] = {
    16, 17, 18, 19,
    5, 4, 3, 2,
    6, 13, 14,
    7, 12, 15,
    0, 11, 8,
    1, 10, 9};

inline float finiteOrZero(float value)
{
  return std::isfinite(value) ? value : 0.0f;
}

inline void rackInputSampleToEngine(const float *rackVolts,
                                    float *engineUnits)
{
  for (int jack = 0; jack < NUM_INPUTS; ++jack)
  {
    engineUnits[JACK_TO_ENGINE_CHANNEL[jack]] =
        finiteOrZero(rackVolts[jack]) * UNITS_PER_VOLT;
  }
}

inline void engineOutputSampleToRack(const float *engineUnits,
                                     float *rackVolts)
{
  for (int channel = 0; channel < NUM_OUTPUTS; ++channel)
  {
    float value = finiteOrZero(engineUnits[channel]);
    value = std::max(-1.0f, std::min(1.0f, value));
    rackVolts[channel] = value * VOLTS_PER_UNIT;
  }
}

inline bool jackMapIsPermutation()
{
  bool seen[NUM_INPUTS] = {};
  for (int jack = 0; jack < NUM_INPUTS; ++jack)
  {
    const int channel = JACK_TO_ENGINE_CHANNEL[jack];
    if (channel < 0 || channel >= NUM_INPUTS || seen[channel])
      return false;
    seen[channel] = true;
  }
  return true;
}

// Converts Rack's sample-at-a-time callback into the ER-301's frame-at-a-time
// callback. The previously rendered frame is emitted while the next frame is
// collected, producing exactly one ER-301 frame of deterministic latency.
class BlockAdapter
{
public:
  BlockAdapter()
  {
    reset();
  }

  void reset(int frameLength = 0)
  {
    mFrameLength = validFrameLength(frameLength) ? frameLength : 0;
    mIndex = 0;
    std::memset(mInputs, 0, sizeof(mInputs));
    std::memset(mOutputs, 0, sizeof(mOutputs));
  }

  int frameLength() const
  {
    return mFrameLength;
  }

  int index() const
  {
    return mIndex;
  }

  template <typename RenderFrame>
  void processSample(const float *rackInputs,
                     float *rackOutputs,
                     int frameLength,
                     RenderFrame renderFrame)
  {
    if (!validFrameLength(frameLength))
    {
      for (int channel = 0; channel < NUM_OUTPUTS; ++channel)
        rackOutputs[channel] = 0.0f;
      return;
    }

    if (mFrameLength != frameLength)
      reset(frameLength);

    float *input = mInputs + mIndex * NUM_INPUTS;
    const float *output = mOutputs + mIndex * NUM_OUTPUTS;
    std::memcpy(input, rackInputs, sizeof(float) * NUM_INPUTS);
    std::memcpy(rackOutputs, output, sizeof(float) * NUM_OUTPUTS);

    ++mIndex;
    if (mIndex >= mFrameLength)
    {
      renderFrame(mInputs, mOutputs);
      mIndex = 0;
    }
  }

private:
  static bool validFrameLength(int frameLength)
  {
    return frameLength > 0 && frameLength <= MAX_FRAME;
  }

  // These staging buffers are copied into the bridge's explicitly aligned
  // engine frames before Pump_callback(). They do not themselves need SIMD
  // alignment. Keeping them naturally aligned prevents the containing Rack
  // Module from becoming an over-aligned C++11 allocation.
  float mInputs[MAX_FRAME * NUM_INPUTS];
  float mOutputs[MAX_FRAME * NUM_OUTPUTS];
  int mFrameLength = 0;
  int mIndex = 0;
};

} // namespace er301audio
