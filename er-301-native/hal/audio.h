#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Audio configuration
#define AUDIO_NUM_CHANNELS 4
#define AUDIO_MAX_OUTPUT_VALUE ((1 << 23) - 1)
#define AUDIO_SAFE_MAX_OUTPUT_VALUE (AUDIO_MAX_OUTPUT_VALUE)

  void Audio_init();
  void Audio_start(void);
  void Audio_stop(void);
  void Audio_restart(void);
  int Audio_getRate(void);
  void Audio_printErrorStatus(void);
  int Audio_getLoad();
  bool Audio_running();
  
  // Called on each frame in the audio thread.
  extern void Audio_callback(int *samples);

#ifdef ER301_VCV_HOST
  // Host-driven audio mode (e.g. hosted inside VCV Rack): the host pulls one
  // frame of AUDIO_NUM_CHANNELS interleaved 24-bit integer samples.
  // Returns false and zeroes the buffer if audio has not been started yet.
  bool Audio_hostRenderFrame(int *samples);
#endif

#ifdef __cplusplus
}
#endif
