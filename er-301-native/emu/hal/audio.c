#include <hal/audio.h>
#include <hal/log.h>
#include <od/config.h>

#ifdef ER301_VCV_HOST
/*
 * ER301_VCV_HOST audio backend
 * ----------------------------
 * When the emulator is hosted inside another audio application (VCV Rack),
 * SDL must not open an audio output device.  Instead, the host pulls audio
 * by calling Audio_hostRenderFrame(), which invokes the existing
 * Audio_callback() to render one complete frame of
 * AUDIO_NUM_CHANNELS (4) interleaved 24-bit integer samples
 * (frame length = globalConfig.frameLength samples per channel).
 *
 * The standalone emulator build (without ER301_VCV_HOST) is unchanged and
 * continues to use the SDL audio backend below.
 */

#include <string.h>
#include <stdatomic.h>

static struct HostAudioLocals
{
  atomic_bool running;
} hostLocal;

void Audio_init()
{
  atomic_init(&hostLocal.running, false);
  logInfo("Audio (ER301_VCV_HOST): SDL audio device disabled; host-driven rendering.");
}

void Audio_restart(void)
{
  Audio_stop();
  Audio_start();
}

void Audio_start(void)
{
  atomic_store_explicit(&hostLocal.running, true, memory_order_release);
  logInfo("Audio (ER301_VCV_HOST): marked running; host will pull frames.");
}

void Audio_stop(void)
{
  atomic_store_explicit(&hostLocal.running, false, memory_order_release);
}

uint32_t Audio_errorCount(void)
{
  return 0;
}

int Audio_getRate(void)
{
  return globalConfig.sampleRate;
}

void Audio_printErrorStatus(void)
{
  logInfo(atomic_load_explicit(&hostLocal.running, memory_order_acquire)
              ? "audio running (host-driven)"
              : "audio stopped (host-driven)");
}

int Audio_getLoad()
{
  return 0;
}

bool Audio_running(void)
{
  return atomic_load_explicit(&hostLocal.running, memory_order_acquire);
}

/*
 * Called by the host's audio thread.  Renders one ER-301 frame into
 * 'samples': FRAMELENGTH * AUDIO_NUM_CHANNELS interleaved ints
 * (OUT1, OUT2, OUT3, OUT4, OUT1, ...), each in [-2^23+1, 2^23-1].
 * Returns false (and leaves the buffer zeroed) if the ER-301 audio
 * system has not been started yet by the application (Lua boot calls
 * app.Audio_start()).
 */
bool Audio_hostRenderFrame(int *samples)
{
  if (!atomic_load_explicit(&hostLocal.running, memory_order_acquire))
  {
    memset(samples, 0, AUDIO_NUM_CHANNELS * FRAMELENGTH * sizeof(int));
    return false;
  }
  Audio_callback(samples);
  return true;
}

#else /* ER301_VCV_HOST */

#include <SDL2/SDL.h>

#define MAX_AUDIO_BUFFER_BYTES (AUDIO_NUM_CHANNELS * MAX_AUDIO_FRAME_LENGTH * sizeof(int))

static struct AudioLocals
{
  int buffer[CACHE_ALIGNED_SIZE(MAX_AUDIO_BUFFER_BYTES) / sizeof(int)] __attribute__((aligned(CACHELINE_SIZE_MAX)));
  SDL_AudioSpec playSpec;
  SDL_AudioDeviceID playDev;
} local;

void playCallback(void *userdata,
                  Uint8 *stream,
                  int len)
{
  // Generate audio.
  Audio_callback(local.buffer);

  // Convert 24-bit to 32-bit and mix down to stereo.
  int *out = (int *)stream;
  for (int i = 0; i < FRAMELENGTH; i++)
  {
    // left channel = OUT1 + OUT3
    out[2 * i] = (local.buffer[4 * i] + local.buffer[4 * i + 2]) << 7;
    // right channel = OUT2 + OUT4
    out[2 * i + 1] = (local.buffer[4 * i + 1] + local.buffer[4 * i + 3]) << 7;
  }
}

void Audio_init()
{
  SDL_InitSubSystem(SDL_INIT_AUDIO);
  SDL_zero(local);

  for (int i = 0; i < SDL_GetNumAudioDrivers(); ++i)
  {
    logInfo("Audio driver %d: %s", i, SDL_GetAudioDriver(i));
  }

#if BUILDOPT_FORCE_ALSA
  if (SDL_AudioInit("alsa"))
  {
    logError("Failed to initialize alsa driver: %s", SDL_GetError());
    logInfo("Opening default driver...");
  }
#endif

  const char *driver_name = SDL_GetCurrentAudioDriver();

  if (driver_name)
  {
    logInfo("Audio subsystem initialized, driver = %s.", driver_name);
  }
  else
  {
    logError("Audio subsystem not initialized.");
  }
}

void Audio_restart(void)
{
  Audio_stop();
  SDL_Delay(200);
  Audio_start();
}

void Audio_start(void)
{
  SDL_AudioSpec want;
  SDL_zero(want);
  want.freq = globalConfig.sampleRate;
  want.format = AUDIO_S32;
  want.channels = 2;
  want.samples = globalConfig.frameLength;
  want.callback = playCallback;

  local.playDev = SDL_OpenAudioDevice(NULL, 0, &want, &local.playSpec, 0);
  if (local.playDev == 0)
  {
    logError("Failed to open audio: %s", SDL_GetError());
  }
  else
  {
    logInfo("Audio Specs: %dHz %dch", local.playSpec.freq, local.playSpec.channels);
    SDL_PauseAudioDevice(local.playDev, 0); /* start audio playing. */
  }
}

void Audio_stop(void)
{
  SDL_CloseAudioDevice(local.playDev);
}

uint32_t Audio_errorCount(void)
{
  return 0;
}

int Audio_getRate(void)
{
  return globalConfig.sampleRate;
}

void Audio_printErrorStatus(void)
{
  switch (SDL_GetAudioDeviceStatus(local.playDev))
  {
  case SDL_AUDIO_STOPPED:
    logInfo("audio stopped");
    break;
  case SDL_AUDIO_PLAYING:
    logInfo("audio playing");
    break;
  case SDL_AUDIO_PAUSED:
    logInfo("audio paused");
    break;
  default:
    logInfo("audio ???");
    break;
  }
}

int Audio_getLoad()
{
  return 0;
}

bool Audio_running()
{
  return SDL_GetAudioDeviceStatus(local.playDev) == SDL_AUDIO_PLAYING;
}
#endif /* ER301_VCV_HOST */
