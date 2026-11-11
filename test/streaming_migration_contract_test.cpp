// Baseline acceptance tests for the ER-301 native-rate streaming migration.
// This uses an identity callback and a sample-by-sample one-pole reference.
// It does NOT exercise Brian's engine or claim a streaming engine exists yet.
#include "../src/er301_audio_contract.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;

void check(bool ok, const char* message, int frame, int phase)
{
  if (!ok)
  {
    std::fprintf(stderr, "FAIL: %s (frame=%d, phase=%d)\n", message, frame, phase);
    ++failures;
  }
}

void testImpulsePhaseAndGate(int frame, int phase, int jack)
{
  er301audio::BlockAdapter adapter;
  const int total = frame * 5 + 3;
  int renderCalls = 0;
  int observed = -1;
  for (int sample = 0; sample < total; ++sample)
  {
    float input[er301audio::NUM_INPUTS] = {};
    float output[er301audio::NUM_OUTPUTS] = {};
    if (sample == phase)
      input[jack] = 1.0f;
    adapter.processSample(input, output, frame,
      [frame, jack, &renderCalls](const float* in, float* out) {
        ++renderCalls;
        for (int s = 0; s < frame; ++s)
        {
          out[s * er301audio::NUM_OUTPUTS] =
              in[s * er301audio::NUM_INPUTS + jack];
          for (int channel = 1; channel < er301audio::NUM_OUTPUTS; ++channel)
            out[s * er301audio::NUM_OUTPUTS + channel] = 0.0f;
        }
      });
    if (std::fabs(output[0]) > 0.5f)
    {
      check(observed == -1, "impulse duplicated", frame, phase);
      observed = sample;
    }
    for (int channel = 1; channel < er301audio::NUM_OUTPUTS; ++channel)
      check(output[channel] == 0.0f, "unexpected output channel", frame, phase);
  }
  check(observed == phase + frame, "input-to-output sample delay changed", frame, phase);
  check(renderCalls == total / frame, "wrong number of frame renders", frame, phase);
}

// Compare an incremental, causal filter against identical arithmetic
// performed by the existing frame adapter. This establishes the target
// numerical behavior for a future streaming implementation of simple DSP.
void testCausalStateAcrossFrames(int frame)
{
  er301audio::BlockAdapter adapter;
  const int total = frame * 6;
  std::vector<float> reference(total, 0.0f);
  std::vector<float> throughBlock(total, 0.0f);
  float referenceState = 0.0f;
  float blockState = 0.0f;
  for (int sample = 0; sample < total; ++sample)
  {
    const float signal = (sample % 29 == 0) ? 1.0f :
                         (sample % 11 == 0) ? -0.25f : 0.0f;
    referenceState = 0.875f * referenceState + 0.125f * signal;
    reference[sample] = referenceState;
    float input[er301audio::NUM_INPUTS] = {};
    float output[er301audio::NUM_OUTPUTS] = {};
    input[8] = signal; // A1 in Rack jack order.
    adapter.processSample(input, output, frame,
      [frame, &blockState](const float* in, float* out) {
        for (int s = 0; s < frame; ++s)
        {
          blockState = 0.875f * blockState +
                       0.125f * in[s * er301audio::NUM_INPUTS + 8];
          out[s * er301audio::NUM_OUTPUTS] = blockState;
          for (int c = 1; c < er301audio::NUM_OUTPUTS; ++c)
            out[s * er301audio::NUM_OUTPUTS + c] = 0.0f;
        }
      });
    throughBlock[sample] = output[0];
  }
  for (int sample = frame; sample < total; ++sample)
  {
    check(std::fabs(throughBlock[sample] - reference[sample - frame]) < 1.0e-6f,
          "stateful DSP is not identical after accounting for frame latency", frame, sample);
  }
}
}

int main()
{
  const int frames[] = {4, 8, 16, 32, 64, 128};
  for (int frame : frames)
  {
    const int phases[] = {0, 1, frame - 1, frame, frame + 1, 2 * frame - 1};
    for (int phase : phases)
    {
      testImpulsePhaseAndGate(frame, phase, 0); // gate input G1
      testImpulsePhaseAndGate(frame, phase, 8); // analog/audio input A1
    }
    testCausalStateAcrossFrames(frame);
  }
  if (failures)
  {
    std::fprintf(stderr, "streaming_migration_contract_test: %d failure(s)\n", failures);
    return 1;
  }
  std::puts("streaming_migration_contract_test: PASS (legacy latency baseline locked)");
  return 0;
}
