#include "../src/er301_audio_contract.hpp"
#include <hal/channels.h>

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
  if (!condition)
  {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

bool near(float a, float b, float tolerance = 1.0e-6f)
{
  return std::fabs(a - b) <= tolerance;
}

void testPhysicalMapping()
{
  using namespace er301audio;
  check(jackMapIsPermutation(), "panel-to-engine map is not a permutation");

  const int expected[NUM_INPUTS] = {
      INPUT_G1, INPUT_G2, INPUT_G3, INPUT_G4,
      INPUT_IN1, INPUT_IN2, INPUT_IN3, INPUT_IN4,
      INPUT_A1, INPUT_A2, INPUT_A3,
      INPUT_B1, INPUT_B2, INPUT_B3,
      INPUT_C1, INPUT_C2, INPUT_C3,
      INPUT_D1, INPUT_D2, INPUT_D3};
  for (int jack = 0; jack < NUM_INPUTS; ++jack)
    check(JACK_TO_ENGINE_CHANNEL[jack] == expected[jack],
          "panel jack maps to the wrong ER-301 channel");
}

void testVoltageConversion()
{
  using namespace er301audio;
  float rack[NUM_INPUTS];
  float engine[NUM_INPUTS];
  for (int jack = 0; jack < NUM_INPUTS; ++jack)
    rack[jack] = -9.5f + (float)jack;

  rack[3] = std::numeric_limits<float>::quiet_NaN();
  rack[7] = std::numeric_limits<float>::infinity();
  rackInputSampleToEngine(rack, engine);

  for (int jack = 0; jack < NUM_INPUTS; ++jack)
  {
    const float expected = std::isfinite(rack[jack]) ? rack[jack] * 0.1f : 0.0f;
    check(near(engine[JACK_TO_ENGINE_CHANNEL[jack]], expected),
          "Rack input scaling, polarity, or non-finite protection failed");
  }

  const float engineOut[NUM_OUTPUTS] = {
      -1.25f, -0.25f, 0.75f,
      std::numeric_limits<float>::quiet_NaN()};
  float rackOut[NUM_OUTPUTS] = {};
  engineOutputSampleToRack(engineOut, rackOut);
  check(near(rackOut[0], -10.0f), "negative output limiter failed");
  check(near(rackOut[1], -2.5f), "negative output polarity/scaling failed");
  check(near(rackOut[2], 7.5f), "positive output polarity/scaling failed");
  check(near(rackOut[3], 0.0f), "non-finite output was not silenced");
}

void testBlockAdapterLatencyAndReset()
{
  using namespace er301audio;
  BlockAdapter adapter;
  const int frame = 4;
  int renderCalls = 0;

  auto render = [&renderCalls](const float *input, float *output) {
    ++renderCalls;
    for (int sample = 0; sample < frame; ++sample)
    {
      for (int channel = 0; channel < NUM_OUTPUTS; ++channel)
      {
        output[sample * NUM_OUTPUTS + channel] =
            input[sample * NUM_INPUTS + channel];
      }
    }
  };

  for (int sample = 0; sample < frame * 2; ++sample)
  {
    float input[NUM_INPUTS] = {};
    float output[NUM_OUTPUTS] = {};
    for (int channel = 0; channel < NUM_OUTPUTS; ++channel)
      input[channel] = (float)(100 * channel + sample + 1);

    adapter.processSample(input, output, frame, render);
    for (int channel = 0; channel < NUM_OUTPUTS; ++channel)
    {
      const float expected = sample < frame
          ? 0.0f
          : (float)(100 * channel + (sample - frame) + 1);
      check(near(output[channel], expected),
            "block adapter is not exactly one frame late");
    }
  }
  check(renderCalls == 2, "block adapter rendered the wrong number of frames");

  adapter.reset();
  float input[NUM_INPUTS] = {};
  float output[NUM_OUTPUTS] = {1, 1, 1, 1};
  adapter.processSample(input, output, frame, render);
  for (int channel = 0; channel < NUM_OUTPUTS; ++channel)
    check(near(output[channel], 0.0f), "block reset leaked stale audio");

  adapter.reset();
  adapter.processSample(input, output, 0, render);
  for (int channel = 0; channel < NUM_OUTPUTS; ++channel)
    check(near(output[channel], 0.0f), "invalid frame length was not muted");
}

void testNativeSampleRateSelection()
{
  using namespace er301audio;
  check(chooseNativeSampleRate(44100) == 48000,
        "44.1 kHz did not select the 48 kHz ER-301 mode");
  check(chooseNativeSampleRate(48000) == 48000,
        "48 kHz did not select the native 48 kHz ER-301 mode");
  check(chooseNativeSampleRate(64000) == 48000,
        "64 kHz did not select the nearer ER-301 mode");
  check(chooseNativeSampleRate(72000) == 96000,
        "72 kHz midpoint policy is inconsistent");
  check(chooseNativeSampleRate(88200) == 96000,
        "88.2 kHz did not select the 96 kHz ER-301 mode");
  check(chooseNativeSampleRate(96000) == 96000,
        "96 kHz did not select the native 96 kHz ER-301 mode");
  check(chooseNativeSampleRate(192000) == 96000,
        "192 kHz did not select the 96 kHz ER-301 mode");
}

} // namespace

int main()
{
  testPhysicalMapping();
  testVoltageConversion();
  testBlockAdapterLatencyAndReset();
  testNativeSampleRateSelection();

  if (failures)
  {
    std::fprintf(stderr, "audio_contract_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("audio_contract_test: PASS\n");
  return 0;
}
