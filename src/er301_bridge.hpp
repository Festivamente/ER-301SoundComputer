#pragma once

#include <atomic>
#include <cstdint>
#include <string>

/**
 * er301_bridge — isolated headless ER-301 engine instances for VCV Rack.
 *
 * The upstream ER-301 engine is intentionally singleton-heavy. Rather than
 * attempting to make every firmware global context-aware, ER-301 Sound Computer gives each
 * Rack module its own loaded image of the engine. Every image therefore owns
 * an independent Emulator, Lua state, task graph, HAL state, display pump,
 * patch serializer, and audio scratch storage.
 *
 * Threading contract for Instance:
 *   - open()/close(), initialize(), savePatchState(), and controls: Rack UI.
 *   - processBlock(): Rack audio thread only; no allocation or blocking.
 *   - display production: engine-image worker; getDisplayFrame(): Rack UI.
 *   - isReady()/audioRunning()/valid(): any thread (atomic slot lookup).
 */
namespace er301
{
  struct RuntimePaths
  {
    std::string bundledRuntimeRoot;
    std::string developmentRuntimeRoot;
    std::string xrootPath;
    std::string userDataRoot;
    std::string frontPath;
    std::string rearPath;
    std::string configPath;
    std::string sessionPath;
    std::string logPath;
    std::string engineImageCacheRoot;
    bool usingDevelopmentFallback = false;
  };

  static constexpr int NUM_INPUTS = 20;
  static constexpr int NUM_OUTPUTS = 4;
  static constexpr int MAX_FRAME = 128;

  static constexpr int MAIN_COLS = 256, MAIN_ROWS = 64;
  static constexpr int SUB_COLS = 128, SUB_ROWS = 64;
  static constexpr int MAIN_BYTES = 2 * (MAIN_COLS * MAIN_ROWS) / 2;
  static constexpr int SUB_BYTES = 2 * (SUB_COLS * SUB_ROWS) / 8;

  enum InputJack
  {
    JACK_G1, JACK_G2, JACK_G3, JACK_G4,
    JACK_IN1, JACK_IN2, JACK_IN3, JACK_IN4,
    JACK_A1, JACK_A2, JACK_A3,
    JACK_B1, JACK_B2, JACK_B3,
    JACK_C1, JACK_C2, JACK_C3,
    JACK_D1, JACK_D2, JACK_D3,
    NUM_INPUT_JACKS
  };

  enum Button
  {
    BTN_M1, BTN_M2, BTN_M3, BTN_M4, BTN_M5, BTN_M6,
    BTN_DIAL1, BTN_CANCEL, BTN_HOME,
    BTN_S1, BTN_S2, BTN_S3,
    BTN_ENTER, BTN_UP, BTN_SHIFT,
    BTN_SELECT1, BTN_SELECT2, BTN_SELECT3, BTN_SELECT4,
    NUM_BUTTONS
  };

  enum PanelLed
  {
    PLED_FINE, PLED_COARSE, PLED_IO, PLED_SAFE,
    PLED_OUT1, PLED_OUT2, PLED_OUT3, PLED_OUT4,
    PLED_LINK12, PLED_LINK23, PLED_LINK34,
    NUM_PANEL_LEDS
  };

  class Instance
  {
  public:
    Instance();
    ~Instance();

    Instance(const Instance &) = delete;
    Instance &operator=(const Instance &) = delete;

    // Claims an idle isolated engine image, loading a new private copy of the
    // ER-301 Sound Computer plugin image when all existing slots are busy.
    bool open(const std::string &imageCacheRoot, std::string &error);
    void close();
    bool valid() const;

    bool initialize(const RuntimePaths &paths, int nativeSampleRate);
    bool isReady() const;
    bool audioRunning() const;
    int frameLength() const;
    int sampleRate() const;
    bool savePatchState(uint32_t timeoutMs = 15000);

    void processBlock(const float *inVolts, float *outVolts);
    bool getDisplayFrame(uint8_t *mainRaw, uint8_t *subRaw);

    // Native desktop file chooser handshake. The ER-301 Lua UI requests a
    // dialog inside its isolated engine; Rack consumes the request on its UI
    // thread and returns the chosen host path.
    bool takeHostFileDialogRequest(std::string &initialPath,
                                   std::string &filterSpec);
    void completeHostFileDialog(const std::string &path);

    // QWERTY-friendly host text entry. The ER-301 Keyboard still validates
    // and commits the returned value inside the firmware UI.
    bool takeHostTextDialogRequest(std::string &message,
                                   std::string &initialText,
                                   bool &extended);
    void completeHostTextDialog(const std::string &text,
                                bool cancelled = false);

    void setButton(Button b, bool pressed);
    void turnEncoder(int delta);
    void setStorageSwitch(int pos);
    void setModeSwitch(int pos);
    bool panelLed(PanelLed led) const;
    void inputLevelLed(int abcdIndex, float *red, float *green) const;

    // Human-readable slot number for diagnostics/context menus. Zero means no
    // engine has been claimed yet.
    unsigned slotNumber() const;

  private:
    std::atomic<void *> mSlot;
  };
}
