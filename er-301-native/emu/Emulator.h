#pragma once

#include <emu/HostRuntimePaths.h>
#include <od/extras/LockFreeQueue.h>
#include <emu/Window.h>
#include <hal/display.h>
#include <SDL2/SDL.h>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>

namespace emu
{
  struct Emulator
  {
    Emulator();
    int run(int argc, char **argv);
    void putDisplayBuffer(DisplayBuffer *buffer);
    DisplayBuffer *getDisplayBuffer();
    int getEncoderValue();
    bool isRearCardPresent();
    bool isFrontCardPresent();

    // ---- Host (e.g. VCV Rack) lifecycle -------------------------------
    // initializeForHost() performs all emulator subsystem setup (SDL video,
    // window, HAL init, interpreter thread).  Must be called from the host
    // UI/main thread.  Passing configFile = 0 uses the default config path.
    bool initializeForHost(const char *configFile = 0);
    // Headless variant: no SDL video subsystem, no SDL window, no SDL event
    // loop.  The host supplies audio I/O, controls, and display presentation.
    bool initializeHeadless(const char *configFile = 0);
    // Headless host variant with explicit read-only and writable runtime
    // paths.  This prevents an embedding host from depending on the current
    // working directory or the standalone emulator's ~/.od defaults.
    bool initializeHeadless(const HostRuntimePaths &paths);
    // Nonblocking: drains SDL events and updates the emulator window once.
    // Must be called periodically from the host UI/main thread.
    // No-op in headless mode.
    void pollHostEvents();
    // Stops the interpreter, saves state, destroys the window.  UI thread.
    void shutdownForHost();
    bool isReady();

    // ---- Headless host I/O --------------------------------------------
    // Requests one display render from the application event loop. Safe to
    // call from the embedding host's dedicated display-pump thread.
    void hostRequestDisplayFrame();
    // Waits for a newly rendered frame, copies the newest raw main/sub buffers,
    // and recycles all superseded buffers. Returns false on timeout or shutdown.
    // mainOut: MAIN_FRAME_BUFFER_BYTES, subOut: SUB_FRAME_BUFFER_BYTES.
    bool hostWaitGetDisplayFrame(uint8_t *mainOut, uint8_t *subOut,
                                 uint32_t timeoutMs);
    // Compatibility nonblocking helper: copies the newest pending frame and
    // requests the next render. The VCV bridge uses the request/wait pair above.
    bool hostGetDisplayFrame(uint8_t *mainOut, uint8_t *subOut);
    // Panel controls (host UI thread).  Buttons use GPIO ids from
    // hal/gpio.h (BUTTON_*).  Pressed = true.
    void hostSetButton(uint32_t id, bool pressed);
    // Encoder delta in raw encoder counts (emulator uses ENCODER_SPEED=5
    // counts per detent-ish step).
    void hostTurnEncoder(int delta);
    // 3-position switches: 1 = up, 2 = mid, 3 = down
    // (matches the emulator's switchState() encoding).
    void hostSetStorageSwitch(int position);
    void hostSetModeSwitch(int position);

  private:
    // Shared setup used by both run() and initializeForHost().
    bool initialize(const char *configFile, bool asHost,
                    bool asHeadless = false,
                    const HostRuntimePaths *hostPaths = 0);
    // One nonblocking iteration of the event/render loop.
    void pollEventsOnce();
    void loop();
    void handleKeyUp(SDL_Keysym sym);
    void handleKeyDown(SDL_Keysym sym);
    void handleMouseButton(SDL_MouseButtonEvent &e);
    void mapButtonToKey(uint32_t id, const std::string &key);

    bool writeDefaultConfiguration(const std::string &filename);
    void loadDefaultConfiguration(const HostRuntimePaths *hostPaths = 0);
    bool loadConfiguration(const std::string &filename);
    std::string xRoot;
    std::string rearRoot;
    std::string frontRoot;
    std::string configRoot;
    std::string sessionFilename;
    std::string configFilename;
    double mouseWheelToKnobFactor;
    double leftRightToKnobFactor;
    double upDownToKnobFactor;
    bool rearCardPresent = true;
    bool frontCardPresent = true;
    bool runtimePathsManagedByHost = false;

    // Persist state between sessions.
    void saveState();
    void restoreState();
    // Shared, idempotent rollback/teardown path. When saveSession is false,
    // partial initialization is cleaned without creating session state.
    void cleanupForHost(bool saveSession);

    Window *window = 0;
    DisplayBuffer ping, pong;
    od::LockFreeQueue<DisplayBuffer *, 4> readyQ, renderQ;
    // Headless display hand-off. The interpreter produces renderQ frames and
    // the host display-pump thread consumes them. This mutex is never touched
    // by the Rack audio thread.
    std::mutex hostDisplayMutex;
    std::condition_variable hostDisplayCondition;
    uint64_t hostDisplayGeneration = 0;
    uint64_t hostDisplayConsumedGeneration = 0;
    uint32_t customEventType = SDL_USEREVENT;
    double encoderValue = 0;
    bool quit = false;
    bool hostMode = false;
    bool headless = false;
    bool initialized = false;
    bool initializing = false;
    bool shuttingDown = false;
    bool sdlInitialized = false;
    bool halInitialized = false;
    uint32_t sdlSubsystems = 0;
    SDL_Thread *interpreterThread = 0;
    bool storageToggleFocused = false;
    bool modeToggleFocused = false;

    // Keyboard Mapping
    std::map<std::string, uint32_t> keyGpioMap;
    std::map<uint32_t, std::string> gpioKeyMap;
    std::string storageToggleFocusKey;
    std::string modeToggleFocusKey;
    std::string zoomInKey;
    std::string zoomOutKey;
    std::string quitKey; // Must be modified with CTRL.

    // Mouse Mapping
    std::map<uint32_t, SDL_Rect> buttonHitMap;
    std::map<uint32_t, SDL_Rect> toggleHitMap;
  };
}