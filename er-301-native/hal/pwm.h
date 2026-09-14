#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#define PWM_NUM_CHANNELS 12

  void Pwm_init(void);
  void Pwm_start(void);
  void Pwm_set(int channel, float value);
  void Pwm_set_raw(int channel, float red, float green);
  // Reads back the current LED PWM levels (implemented by the emulator HAL;
  // used by hosting applications such as the VCV Rack module).
  void Pwm_hostGet(int channel, float *red, float *green);

#ifdef __cplusplus
}
#endif
