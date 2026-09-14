#include <od/tasks/InputTask.h>
#include <od/tasks/OutputTask.h>
#include <od/objects/Inlet.h>
#include <od/objects/Outlet.h>
#include <od/config.h>
#include <hal/channels.h>
#include <hal/constants.h>

#include <cmath>
#include <cstdio>
#include <vector>

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

void testInputTaskChannelExtraction()
{
  using namespace od;
  InputTask *task = new InputTask();
  task->attach();

  Outlet *outlets[NUM_INPUT_CHANNELS] = {
      &task->mA1, &task->mA2, &task->mA3,
      &task->mB1, &task->mB2, &task->mB3,
      &task->mC1, &task->mC2, &task->mC3,
      &task->mD1, &task->mD2, &task->mD3,
      &task->mG1, &task->mG2, &task->mG3, &task->mG4,
      &task->mIN1, &task->mIN2, &task->mIN3, &task->mIN4};
  const int channels[NUM_INPUT_CHANNELS] = {
      INPUT_A1, INPUT_A2, INPUT_A3,
      INPUT_B1, INPUT_B2, INPUT_B3,
      INPUT_C1, INPUT_C2, INPUT_C3,
      INPUT_D1, INPUT_D2, INPUT_D3,
      INPUT_G1, INPUT_G2, INPUT_G3, INPUT_G4,
      INPUT_IN1, INPUT_IN2, INPUT_IN3, INPUT_IN4};

  std::vector<Inlet *> consumers;
  for (int i = 0; i < NUM_INPUT_CHANNELS; ++i)
  {
    Inlet *consumer = new Inlet("test consumer");
    consumer->attach();
    consumer->connect(outlets[i]);
    consumers.push_back(consumer);
  }

  std::vector<float> inputs(globalConfig.frameLength * NUM_INPUT_CHANNELS);
  std::vector<float> unused(globalConfig.frameLength * NUM_OUTPUT_CHANNELS, 0.0f);
  for (int sample = 0; sample < globalConfig.frameLength; ++sample)
    for (int channel = 0; channel < NUM_INPUT_CHANNELS; ++channel)
      inputs[sample * NUM_INPUT_CHANNELS + channel] =
          (float)(sample * 1000 + channel);

  task->process(inputs.data(), unused.data());
  for (int named = 0; named < NUM_INPUT_CHANNELS; ++named)
  {
    const float *buffer = outlets[named]->buffer();
    for (int sample = 0; sample < globalConfig.frameLength; ++sample)
    {
      const float expected = (float)(sample * 1000 + channels[named]);
      check(near(buffer[sample], expected),
            "InputTask extracted the wrong physical channel");
    }
    check(near(task->mLastInput[channels[named]],
               (float)((globalConfig.frameLength - 1) * 1000 + channels[named])),
          "InputTask last-sample cache uses the wrong channel");
  }

  for (Inlet *consumer : consumers)
  {
    consumer->disconnect();
    consumer->release();
  }
  task->release();
}

void fillOutlet(od::Outlet *outlet, int channel)
{
  float *buffer = outlet->buffer();
  for (int sample = 0; sample < globalConfig.frameLength; ++sample)
    buffer[sample] = (float)(100 * channel + sample + 1);
}

void testOutputTaskInterleaveAndSilence()
{
  using namespace od;
  OutputTask *task = new OutputTask();
  task->attach();

  std::vector<float> inputs(globalConfig.frameLength * NUM_INPUT_CHANNELS, 0.0f);
  std::vector<float> outputs(globalConfig.frameLength * NUM_OUTPUT_CHANNELS, 1234.0f);

  // All outputs disconnected: every physical output must be zero. This test
  // specifically prevents the historical OUT4 off-by-one regression.
  task->process(inputs.data(), outputs.data());
  for (float value : outputs)
    check(near(value, 0.0f), "disconnected OutputTask channel leaked stale audio");

  Outlet *sources[NUM_OUTPUT_CHANNELS];
  Inlet *destinations[NUM_OUTPUT_CHANNELS] = {
      &task->mOut1, &task->mOut2, &task->mOut3, &task->mOut4};
  for (int channel = 0; channel < NUM_OUTPUT_CHANNELS; ++channel)
  {
    sources[channel] = new Outlet("test source");
    sources[channel]->attach();
    fillOutlet(sources[channel], channel);
    destinations[channel]->connect(sources[channel]);
  }

  std::fill(outputs.begin(), outputs.end(), -999.0f);
  task->process(inputs.data(), outputs.data());
  for (int sample = 0; sample < globalConfig.frameLength; ++sample)
    for (int channel = 0; channel < NUM_OUTPUT_CHANNELS; ++channel)
      check(near(outputs[sample * NUM_OUTPUT_CHANNELS + channel],
                 (float)(100 * channel + sample + 1)),
            "OutputTask interleaved a physical output incorrectly");

  // Disconnect only OUT4. OUT1-3 must remain untouched and OUT4 must become
  // zero at every sample. The old code zeroed channel index 4, which actually
  // corrupted OUT1 of the following samples and left OUT4 stale.
  destinations[3]->disconnect();
  std::fill(outputs.begin(), outputs.end(), -999.0f);
  task->process(inputs.data(), outputs.data());
  for (int sample = 0; sample < globalConfig.frameLength; ++sample)
  {
    for (int channel = 0; channel < 3; ++channel)
      check(near(outputs[sample * NUM_OUTPUT_CHANNELS + channel],
                 (float)(100 * channel + sample + 1)),
            "disconnecting OUT4 corrupted another output");
    check(near(outputs[sample * NUM_OUTPUT_CHANNELS + 3], 0.0f),
          "disconnected OUT4 was not silenced");
  }

  for (int channel = 0; channel < NUM_OUTPUT_CHANNELS; ++channel)
  {
    destinations[channel]->disconnect();
    sources[channel]->release();
  }
  task->release();
}

} // namespace

int main()
{
  globalConfig.frameLength = 8;
  globalConfig.sampleRate = 48000;
  globalConfig.samplePeriod = 1.0f / 48000.0f;
  globalConfig.frameRate = 6000.0f;
  globalConfig.framePeriod = 8.0f / 48000.0f;

  testInputTaskChannelExtraction();
  testOutputTaskInterleaveAndSilence();

  if (failures)
  {
    std::fprintf(stderr, "task_io_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("task_io_test: PASS\n");
  return 0;
}
