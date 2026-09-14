#include <od/config.h>
#include <hal/card.h>

#include <math.h>
#include <stdio.h>

bool Card_mount(uint32_t drive)
{
  (void)drive;
  return false;
}

int main(void)
{
  globalConfig.frameLength = 128;

  if (!Config_setSampleRate(48000) || globalConfig.sampleRate != 48000 ||
      fabsf(globalConfig.samplePeriod - 1.0f / 48000.0f) > 1.0e-10f ||
      globalConfig.frameRate != 375 ||
      fabsf(globalConfig.framePeriod - 128.0f / 48000.0f) > 1.0e-9f)
  {
    fprintf(stderr, "FAIL: 48 kHz configuration override\n");
    return 1;
  }

  if (!Config_setSampleRate(96000) || globalConfig.sampleRate != 96000 ||
      fabsf(globalConfig.samplePeriod - 1.0f / 96000.0f) > 1.0e-10f ||
      globalConfig.frameRate != 750 ||
      fabsf(globalConfig.framePeriod - 128.0f / 96000.0f) > 1.0e-9f)
  {
    fprintf(stderr, "FAIL: 96 kHz configuration override\n");
    return 1;
  }

  if (Config_setSampleRate(44100) || globalConfig.sampleRate != 96000)
  {
    fprintf(stderr, "FAIL: invalid native rate was accepted\n");
    return 1;
  }

  printf("config_rate_test: PASS\n");
  return 0;
}
