#pragma once

namespace emu
{
  /**
   * Explicit runtime paths supplied by an embedding host.
   *
   * The Emulator copies these strings during initialization; callers only
   * need to keep the pointed-to character data alive for the duration of the
   * initialization call.
   */
  struct HostRuntimePaths
  {
    const char *xRoot;
    const char *rearRoot;
    const char *frontRoot;
    const char *configFile;
    const char *sessionFile;
    int sampleRate; // 48000 or 96000; 0 keeps firmware.cfg/default
  };
}
