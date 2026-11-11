#include "../src/er301_streaming_rate_adapter.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {
int failures = 0;

void check(bool ok, const char *message)
{
  if (!ok)
  {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

struct CopyEngine
{
  int renderCalls = 0;
  void operator()(const float *input, float *output)
  {
    ++renderCalls;
    for (int c = 0; c < er301audio::NUM_OUTPUTS; ++c)
      output[c] = input[c];
  }
};

void testImpulse(int rackRate, int engineRate, int frameLength, int onsetIndex)
{
  er301audio::StreamingRateAdapter adapter;
  adapter.configure(rackRate, engineRate, frameLength);
  CopyEngine engine;
  int observed = -1;
  for (int i = 0; i < 2200; ++i)
  {
    float in[er301audio::NUM_INPUTS] = {};
    float out[er301audio::NUM_OUTPUTS] = {};
    if (i >= onsetIndex && i < onsetIndex + std::max(12, rackRate / engineRate * 4))
      in[0] = 1.f;
    adapter.processSample(in, out, [&engine](const float *input, float *output) { engine(input, output); });
    if (observed < 0 && std::fabs(out[0]) > 0.05f)
      observed = i;
    for (int c = 0; c < er301audio::NUM_OUTPUTS; ++c)
      check(std::isfinite(out[c]), "nonfinite output during impulse");
  }
  // The SRC itself adds one ER-301 sample of causal interpolation delay.
  // Allow a few host samples for phase rounding at arbitrary rate ratios.
  const int maximalLatency =
      (rackRate + engineRate - 1) / engineRate + 5;
  check(observed >= onsetIndex && observed <= onsetIndex + maximalLatency,
        "impulse was delayed excessively or lost");
  check(engine.renderCalls > 0, "engine render callback never ran");
}

void testConstantAndIsolation(int rackRate, int engineRate, int frameLength)
{
  er301audio::StreamingRateAdapter adapter;
  adapter.configure(rackRate, engineRate, frameLength);
  CopyEngine engine;
  float out[er301audio::NUM_OUTPUTS] = {};
  for (int i = 0; i < 4000; ++i)
  {
    float in[er301audio::NUM_INPUTS] = {};
    in[0] = 0.35f;
    in[1] = -0.75f;
    in[2] = 0.6f;
    in[3] = 0.f;
    adapter.processSample(in, out, [&engine](const float *input, float *output) { engine(input, output); });
    for (int c = 0; c < er301audio::NUM_OUTPUTS; ++c)
      check(std::isfinite(out[c]), "nonfinite output during constant input");
  }
  check(std::fabs(out[0] - 0.35f) < 0.0001f, "constant channel 0 altered");
  check(std::fabs(out[1] + 0.75f) < 0.0001f, "constant channel 1 altered");
  check(std::fabs(out[2] - 0.6f) < 0.0001f, "constant channel 2 altered");
  check(std::fabs(out[3]) < 0.0001f, "channel 3 leaked");
}

void testNarrowGateAtDownsampledPhase()
{
  er301audio::StreamingRateAdapter adapter;
  adapter.configure(96000, 48000, 4);
  CopyEngine engine;
  float peak = 0.f;
  for (int i = 0; i < 200; ++i)
  {
    float in[er301audio::NUM_INPUTS] = {};
    float out[er301audio::NUM_OUTPUTS] = {};
    // A single host-sample-wide G1 gate at an odd 96 kHz sample must not be
    // silently discarded when the engine ticks on even host samples.
    if (i == 81)
      in[0] = 10.f;
    adapter.processSample(in, out,
        [&engine](const float *a, float *b) { engine(a, b); });
    peak = std::max(peak, out[0]);
  }
  check(peak > 1.f, "one-sample G1 rising edge lost in downsampling");
}

void testRateChanges()
{
  er301audio::StreamingRateAdapter adapter;
  CopyEngine engine;
  for (int rackRate : {44100, 96000, 48000, 88200, 192000})
  {
    adapter.configure(rackRate, 48000, 4);
    for (int i = 0; i < 150; ++i)
    {
      float in[er301audio::NUM_INPUTS] = {};
      float out[er301audio::NUM_OUTPUTS] = {};
      in[0] = std::sin(2.f * 3.14159265f * 440.f * i / rackRate);
      adapter.processSample(in, out, [&engine](const float *input, float *output) { engine(input, output); });
      check(std::isfinite(out[0]), "nonfinite output after rate change");
    }
  }
}
} // namespace

int main()
{
  for (int frameLength : {4, 16, 32, 128})
  {
    for (int rackRate : {44100, 96000, 88200, 192000})
    {
      const int engineRate = rackRate == 88200 ? 96000 : 48000;
      testImpulse(rackRate, engineRate, frameLength, 199);
      testImpulse(rackRate, engineRate, frameLength, 200);
      testConstantAndIsolation(rackRate, engineRate, frameLength);
    }
  }
  testRateChanges();
  testNarrowGateAtDownsampledPhase();
  if (failures)
  {
    std::fprintf(stderr, "%d test assertions failed\n", failures);
    return 1;
  }
  std::puts("StreamingRateAdapter causal latency, channel and rate tests passed");
  return 0;
}
