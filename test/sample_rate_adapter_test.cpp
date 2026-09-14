#include "../src/er301_sample_rate_adapter.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

// Minimal deterministic Speex API stand-in for testing ER-301 Sound Computer's fixed-buffer
// scheduling and queueing. Production uses Rack's real Speex implementation.
struct SpeexResamplerState_
{
  unsigned channels;
  unsigned inRate;
  unsigned outRate;
  unsigned inputStride = 1;
  unsigned outputStride = 1;
  std::vector<unsigned long long> remainder;
};

extern "C" {
SpeexResamplerState *speex_resampler_init(spx_uint32_t channels,
                                          spx_uint32_t inRate,
                                          spx_uint32_t outRate,
                                          int quality,
                                          int *err)
{
  (void)quality;
  SpeexResamplerState *state = new SpeexResamplerState;
  state->channels = channels;
  state->inRate = inRate;
  state->outRate = outRate;
  state->remainder.assign(channels, 0);
  if (err)
    *err = RESAMPLER_ERR_SUCCESS;
  return state;
}

void speex_resampler_destroy(SpeexResamplerState *state)
{
  delete state;
}

void speex_resampler_set_input_stride(SpeexResamplerState *state,
                                      spx_uint32_t stride)
{
  state->inputStride = stride;
}

void speex_resampler_set_output_stride(SpeexResamplerState *state,
                                       spx_uint32_t stride)
{
  state->outputStride = stride;
}

int speex_resampler_reset_mem(SpeexResamplerState *state)
{
  std::fill(state->remainder.begin(), state->remainder.end(), 0);
  return RESAMPLER_ERR_SUCCESS;
}

int speex_resampler_process_float(SpeexResamplerState *state,
                                  spx_uint32_t channel,
                                  const float *input,
                                  spx_uint32_t *inputLength,
                                  float *output,
                                  spx_uint32_t *outputLength)
{
  const unsigned inputFrames = *inputLength;
  const unsigned outputCapacity = *outputLength;
  const unsigned long long numerator = state->remainder[channel] +
      (unsigned long long)inputFrames * state->outRate;
  const unsigned requested = (unsigned)(numerator / state->inRate);
  const unsigned produced = std::min(requested, outputCapacity);

  for (unsigned i = 0; i < produced; ++i)
  {
    const unsigned source = inputFrames > 0
        ? std::min(inputFrames - 1,
                   (unsigned)(((unsigned long long)i * state->inRate) /
                              state->outRate))
        : 0;
    output[i * state->outputStride] =
        inputFrames > 0 ? input[source * state->inputStride] : 0.0f;
  }

  *inputLength = inputFrames;
  *outputLength = produced;
  state->remainder[channel] = numerator -
      (unsigned long long)produced * state->inRate;
  return RESAMPLER_ERR_SUCCESS;
}
}

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

void runRateCase(int rackRate, int engineRate)
{
  er301audio::SampleRateAdapter adapter;
  const int frameLength = 128;
  adapter.configure(rackRate, engineRate, frameLength);

  long long renderedFrames = 0;
  int renderCalls = 0;
  const int rackFrames = rackRate * 2;
  int nonZeroOutputs = 0;

  for (int sample = 0; sample < rackFrames; ++sample)
  {
    float input[er301audio::NUM_INPUTS] = {};
    float output[er301audio::NUM_OUTPUTS] = {};
    for (int channel = 0; channel < er301audio::NUM_OUTPUTS; ++channel)
      input[channel] = (float)(channel + 1);

    adapter.processSample(input, output,
        [&](const float *engineInput, float *engineOutput) {
          ++renderCalls;
          renderedFrames += frameLength;
          for (int i = 0; i < frameLength; ++i)
          {
            for (int channel = 0;
                 channel < er301audio::NUM_OUTPUTS; ++channel)
            {
              engineOutput[i * er301audio::NUM_OUTPUTS + channel] =
                  engineInput[i * er301audio::NUM_INPUTS + channel];
            }
          }
        });

    for (int channel = 0; channel < er301audio::NUM_OUTPUTS; ++channel)
    {
      if (std::fabs(output[channel]) > 0.5f)
        ++nonZeroOutputs;
    }
  }

  const long long expectedEngineFrames =
      (long long)rackFrames * engineRate / rackRate;
  check(std::llabs(renderedFrames - expectedEngineFrames) <= frameLength * 2,
        "engine-frame production drifted at a converted sample rate");
  check(renderCalls > 0, "sample-rate adapter never rendered an ER-301 block");
  check(nonZeroOutputs > rackFrames * 2,
        "converted output remained silent after startup latency");
  check(adapter.droppedOutputFrames() == 0,
        "sample-rate adapter overflowed its fixed output queue");

  adapter.reset();
  float zeroInput[er301audio::NUM_INPUTS] = {};
  float output[er301audio::NUM_OUTPUTS] = {1, 1, 1, 1};
  adapter.processSample(zeroInput, output,
      [](const float *, float *) {});
  for (int channel = 0; channel < er301audio::NUM_OUTPUTS; ++channel)
    check(output[channel] == 0.0f,
          "sample-rate adapter reset leaked stale output");
}
}

int main()
{
  runRateCase(44100, 48000);
  runRateCase(88200, 96000);
  runRateCase(192000, 96000);

  if (failures)
  {
    std::fprintf(stderr, "sample_rate_adapter_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("sample_rate_adapter_test: PASS\n");
  return 0;
}
