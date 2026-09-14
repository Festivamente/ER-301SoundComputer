#include "er301_bridge.hpp"
#include "er301_audio_contract.hpp"

#include <rack.hpp>

#include <emu/emu.h>
#include <emu/HostFileDialog.h>
#include <emu/HostTextDialog.h>
#include <hal/audio.h>
#include <hal/constants.h>
#include <hal/channels.h>
#include <hal/gpio.h>
#include <hal/pump.h>
#include <hal/pwm.h>
#include <hal/display.h>
#include <hal/events.h>
#include <od/config.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#endif

static_assert(er301::NUM_INPUTS == NUM_INPUT_CHANNELS, "input count mismatch");
static_assert(er301::NUM_OUTPUTS == NUM_OUTPUT_CHANNELS, "output count mismatch");
static_assert(er301::MAX_FRAME == MAX_AUDIO_FRAME_LENGTH, "frame size mismatch");
static_assert(er301::MAIN_BYTES == MAIN_FRAME_BUFFER_BYTES, "main fb size mismatch");
static_assert(er301::SUB_BYTES == SUB_FRAME_BUFFER_BYTES, "sub fb size mismatch");

namespace
{
  static const uint32_t kBridgeAbiVersion = 1;

  struct RuntimePathsC
  {
    const char *xrootPath;
    const char *userDataRoot;
    const char *frontPath;
    const char *rearPath;
    const char *configPath;
    const char *sessionPath;
    const char *logPath;
  };

  struct BridgeApiV1
  {
    uint32_t abiVersion;
    uint32_t structSize;
    const void *(*imageCookie)();
    bool (*acquire)();
    void (*release)();
    bool (*initialize)(const RuntimePathsC *, int);
    bool (*isReady)();
    bool (*audioRunning)();
    int (*frameLength)();
    int (*sampleRate)();
    bool (*savePatchState)(uint32_t);
    void (*processBlock)(const float *, float *);
    bool (*getDisplayFrame)(uint8_t *, uint8_t *);
    void (*setButton)(int, bool);
    void (*turnEncoder)(int);
    void (*setStorageSwitch)(int);
    void (*setModeSwitch)(int);
    bool (*panelLed)(int);
    void (*inputLevelLed)(int, float *, float *);
    bool (*takeHostFileDialogRequest)(const char **, const char **);
    void (*completeHostFileDialog)(const char *);
    bool (*takeHostTextDialogRequest)(const char **, const char **, int *);
    void (*completeHostTextDialog)(const char *);
  };

#if defined(_WIN32)
#define ER301SoundComputer_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define ER301SoundComputer_EXPORT __attribute__((visibility("default")))
#else
#define ER301SoundComputer_EXPORT
#endif

  const BridgeApiV1 *localBridgeApi();

  static bool ensureDirectoryRecursive(const std::string &path,
                                       std::string &error)
  {
    if (path.empty())
    {
      error = "Engine image cache path is empty.";
      return false;
    }

#ifdef _WIN32
    if (rack::system::exists(path))
      return true;

    if (!rack::system::createDirectories(path))
    {
      error = "Could not create engine image directory: " + path;
      return false;
    }

    return true;
#else
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0)
    {
      if (S_ISDIR(st.st_mode))
        return true;
      error = "Engine image cache path is not a directory: " + path;
      return false;
    }

    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0)
    {
      if (!ensureDirectoryRecursive(path.substr(0, slash), error))
        return false;
    }

    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
    {
      error = "Could not create engine image directory " + path + ": " +
              std::strerror(errno);
      return false;
    }

    return true;
#endif
  }

  static bool copyImageAtomically(const std::string &source,
                                  const std::string &destination,
                                  std::string &error)
  {
    std::ifstream input(source.c_str(), std::ios::binary);
    if (!input)
    {
      error = "Could not open ER-301 Sound Computer engine image: " + source;
      return false;
    }

    const std::string temporary = destination + ".tmp";
    std::ofstream output(temporary.c_str(),
                         std::ios::binary | std::ios::trunc);
    if (!output)
    {
      error = "Could not create private engine image: " + temporary;
      return false;
    }
    output << input.rdbuf();
    output.flush();
    if (!output.good())
    {
      error = "Could not finish private engine image: " + temporary;
      output.close();
      ::unlink(temporary.c_str());
      return false;
    }
    output.close();

    struct stat sourceStat{};
    if (::stat(source.c_str(), &sourceStat) == 0)
      ::chmod(temporary.c_str(), sourceStat.st_mode & 0777);
    else
      ::chmod(temporary.c_str(), 0755);

    ::unlink(destination.c_str());
    if (::rename(temporary.c_str(), destination.c_str()) != 0)
    {
      error = "Could not install private engine image " + destination +
              ": " + std::strerror(errno);
      ::unlink(temporary.c_str());
      return false;
    }
    return true;
  }

  static std::string currentImagePath(std::string &error)
  {
#if defined(_WIN32)
    HMODULE module = nullptr;

    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&kBridgeAbiVersion),
            &module))
    {
      std::ostringstream message;
      message << "GetModuleHandleEx could not locate the ER-301 Sound Computer plugin image "
              << "(Windows error " << GetLastError() << ").";
      error = message.str();
      return std::string();
    }

    std::vector<char> imagePath(32768, '\0');

    const DWORD length =
        GetModuleFileNameA(module,
                           imagePath.data(),
                           static_cast<DWORD>(imagePath.size()));

    if (length == 0 ||
        static_cast<std::size_t>(length) >= imagePath.size())
    {
      std::ostringstream message;
      message << "GetModuleFileName could not locate the ER-301 Sound Computer plugin image "
              << "(Windows error " << GetLastError() << ").";
      error = message.str();
      return std::string();
    }

    return std::string(imagePath.data(), length);

#elif defined(__APPLE__) || defined(__linux__)
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void *>(&localBridgeApi), &info) == 0 ||
        info.dli_fname == nullptr)
    {
      error = "dladdr could not locate the ER-301 Sound Computer plugin image.";
      return std::string();
    }
    return info.dli_fname;
#else
    error = "Private engine images are not implemented on this platform.";
    return std::string();
#endif
  }

#if defined(__APPLE__)
  // Rack plugins are packaged with an absolute dependency on
  // /tmp/Rack2/libRack.dylib. Rack creates this link while initially loading
  // plugins, but it may no longer exist when a second ER-301 Sound Computer image is dlopen'd
  // later from the user-data cache. Recreate Rack's documented loader link
  // from the libRack image that is already present in this process.
  static bool ensureDarwinRackLoaderLink(std::string &error)
  {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void *>(&rack::system::getTime), &info) == 0 ||
        info.dli_fname == nullptr)
    {
      error = "dladdr could not locate the loaded libRack.dylib image.";
      return false;
    }

    const std::string rackLibraryPath(info.dli_fname);
    const std::size_t slash = rackLibraryPath.find_last_of('/');
    if (slash == std::string::npos || slash == 0)
    {
      error = "Could not determine libRack.dylib's containing directory: " +
              rackLibraryPath;
      return false;
    }
    const std::string rackLibraryDirectory = rackLibraryPath.substr(0, slash);
    const std::string loaderDirectory = "/tmp/Rack2";
    const std::string loaderLibrary = loaderDirectory + "/libRack.dylib";

    struct stat st{};
    if (::lstat(loaderDirectory.c_str(), &st) == 0)
    {
      // A valid existing Rack link/directory is preferable to changing a
      // process-wide loader path. A stale symlink is safe to replace; never
      // remove a real directory or another filesystem object.
      if (::access(loaderLibrary.c_str(), R_OK) == 0)
        return true;
      if (S_ISLNK(st.st_mode))
      {
        if (::unlink(loaderDirectory.c_str()) != 0)
        {
          error = "Could not replace stale /tmp/Rack2 symlink: " +
                  std::string(std::strerror(errno));
          return false;
        }
      }
      else
      {
        error = "/tmp/Rack2 exists but does not expose libRack.dylib.";
        return false;
      }
    }
    else if (errno != ENOENT)
    {
      error = "Could not inspect /tmp/Rack2: " +
              std::string(std::strerror(errno));
      return false;
    }

    if (::symlink(rackLibraryDirectory.c_str(), loaderDirectory.c_str()) != 0)
    {
      // Another thread/process may have created it between lstat and symlink.
      if (errno == EEXIST && ::access(loaderLibrary.c_str(), R_OK) == 0)
        return true;
      error = "Could not create Rack loader link /tmp/Rack2 -> " +
              rackLibraryDirectory + ": " + std::strerror(errno);
      return false;
    }

    if (::access(loaderLibrary.c_str(), R_OK) != 0)
    {
      error = "Rack loader link was created but libRack.dylib is unreadable.";
      return false;
    }
    return true;
  }
#endif

  static void closePrivateImage(void *handle)
  {
    if (!handle)
      return;

#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
  }

  struct EngineSlot
  {
    void *imageHandle = nullptr; // null for the plugin's original image
    const BridgeApiV1 *api = nullptr;
    const void *cookie = nullptr;
    bool busy = false;
    unsigned number = 0;
    std::string imagePath;
  };

  static std::mutex &slotMutex()
  {
    static std::mutex mutex;
    return mutex;
  }

  static std::vector<std::unique_ptr<EngineSlot> > &engineSlots()
  {
    static std::vector<std::unique_ptr<EngineSlot> > slots;
    return slots;
  }

  static EngineSlot *claimEngineSlot(const std::string &cacheRoot,
                                     std::string &error)
  {
    std::lock_guard<std::mutex> lock(slotMutex());
    std::vector<std::unique_ptr<EngineSlot> > &slots = engineSlots();

    if (slots.empty())
    {
      std::unique_ptr<EngineSlot> local(new EngineSlot());
      local->api = localBridgeApi();
      local->cookie = local->api->imageCookie();
      local->number = 1;
      local->imagePath = "plugin image";
      slots.push_back(std::move(local));
    }

    for (std::size_t i = 0; i < slots.size(); ++i)
    {
      EngineSlot *slot = slots[i].get();
      if (slot->busy)
        continue;
      if (!slot->api->acquire())
      {
        error = "An idle ER-301 engine image refused ownership.";
        return nullptr;
      }
      slot->busy = true;
      return slot;
    }

#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    if (!ensureDirectoryRecursive(cacheRoot, error))
      return nullptr;

    std::string source = currentImagePath(error);
    if (source.empty())
      return nullptr;

    const unsigned number = static_cast<unsigned>(slots.size() + 1);
    std::ostringstream folderName;
    folderName << "slot-" << number;
    const std::string slotDirectory = cacheRoot + "/" + folderName.str();
    if (!ensureDirectoryRecursive(slotDirectory, error))
      return nullptr;

#if defined(_WIN32)
    const char *filename = "ER-301SoundComputerEngine.dll";
#elif defined(__APPLE__)
    const char *filename = "ER-301SoundComputerEngine.dylib";
#else
    const char *filename = "ER-301SoundComputerEngine.so";
#endif
    const std::string destination = slotDirectory + "/" + filename;
    if (!copyImageAtomically(source, destination, error))
      return nullptr;

#if defined(__APPLE__)
    if (!ensureDarwinRackLoaderLink(error))
      return nullptr;
#endif

    typedef const BridgeApiV1 *(*GetApiFn)();

    void *handle = nullptr;
    GetApiFn getApi = nullptr;

#if defined(_WIN32)
    HMODULE module = LoadLibraryA(destination.c_str());

    if (!module)
    {
      std::ostringstream message;
      message << "Could not load private ER-301 engine image "
              << destination
              << " (Windows error " << GetLastError() << ").";
      error = message.str();
      return nullptr;
    }

    handle = reinterpret_cast<void *>(module);

    FARPROC symbol = GetProcAddress(module, "soundcomputer_engine_api_v1");
    if (symbol)
    {
      // Win32 exposes function addresses as FARPROC. Copying the pointer
      // representation avoids GCC's incompatible-function-pointer cast warning
      // while preserving the API-mandated GetProcAddress conversion.
      static_assert(sizeof(getApi) == sizeof(symbol),
                    "Unexpected Windows function pointer size");
      std::memcpy(&getApi, &symbol, sizeof(getApi));
    }

    if (!getApi)
    {
      std::ostringstream message;
      message << "Private engine image does not export the ER-301 Sound Computer bridge API "
              << "(Windows error " << GetLastError() << ").";
      error = message.str();
      closePrivateImage(handle);
      return nullptr;
    }

#else
    int loadFlags = RTLD_NOW | RTLD_LOCAL;

#if defined(__APPLE__) && defined(RTLD_FIRST)
    // Restrict handle-based symbol lookup to the copied image before its
    // dependencies. The cookie check below still verifies actual data-image
    // isolation rather than trusting loader flags alone.
    loadFlags |= RTLD_FIRST;
#endif

    handle = dlopen(destination.c_str(), loadFlags);

    if (!handle)
    {
      const char *message = dlerror();
      error = "Could not load private ER-301 engine image: ";
      error += message ? message : "unknown dlopen error";
      return nullptr;
    }

    dlerror();

    getApi = reinterpret_cast<GetApiFn>(
        dlsym(handle, "soundcomputer_engine_api_v1"));

    const char *symbolError = dlerror();

    if (symbolError || !getApi)
    {
      error = "Private engine image does not export the ER-301 Sound Computer bridge API: ";
      error += symbolError ? symbolError : "missing function";
      closePrivateImage(handle);
      return nullptr;
    }
#endif

    const BridgeApiV1 *api = getApi();
    if (!api || api->abiVersion != kBridgeAbiVersion ||
        api->structSize < sizeof(BridgeApiV1))
    {
      error = "Private ER-301 engine image has an incompatible bridge ABI.";
      closePrivateImage(handle);
      return nullptr;
    }

    const void *cookie = api->imageCookie();
    for (std::size_t i = 0; i < slots.size(); ++i)
    {
      if (slots[i]->cookie == cookie)
      {
        // Some dynamic loaders may coalesce byte-identical libraries. Never
        // pretend that such a handle is isolated: doing so would revive the
        // exact singleton collisions that private engine images are designed to remove.
        error = "The operating system reused an existing ER-301 Sound Computer image instead "
                "of loading an isolated engine copy.";
        closePrivateImage(handle);
        return nullptr;
      }
    }

    if (!api->acquire())
    {
      error = "The new private ER-301 engine image could not be acquired.";
      closePrivateImage(handle);
      return nullptr;
    }

    std::unique_ptr<EngineSlot> slot(new EngineSlot());
    slot->imageHandle = handle;
    slot->api = api;
    slot->cookie = cookie;
    slot->busy = true;
    slot->number = number;
    slot->imagePath = destination;
    EngineSlot *result = slot.get();
    slots.push_back(std::move(slot));
    return result;
#else
    error = "No additional isolated ER-301 engine image is available.";
    return nullptr;
#endif
  }

  static void returnEngineSlot(EngineSlot *slot)
  {
    if (!slot)
      return;

    // Keep the image loaded for the Rack process lifetime. SWIG modules retain
    // process-static linkage data, and unloading/reloading them was the source
    // of the original lifetime crash. A returned slot is cleanly shut down and reused.
    slot->api->release();
    std::lock_guard<std::mutex> lock(slotMutex());
    slot->busy = false;
  }
}

namespace er301
{
  static_assert(er301audio::NUM_INPUTS == NUM_INPUTS, "audio input count mismatch");
  static_assert(er301audio::NUM_OUTPUTS == NUM_OUTPUTS, "audio output count mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_G1] == INPUT_G1, "G1 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_G2] == INPUT_G2, "G2 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_G3] == INPUT_G3, "G3 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_G4] == INPUT_G4, "G4 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_IN1] == INPUT_IN1, "IN1 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_IN2] == INPUT_IN2, "IN2 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_IN3] == INPUT_IN3, "IN3 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_IN4] == INPUT_IN4, "IN4 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_A1] == INPUT_A1, "A1 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_A2] == INPUT_A2, "A2 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_A3] == INPUT_A3, "A3 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_B1] == INPUT_B1, "B1 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_B2] == INPUT_B2, "B2 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_B3] == INPUT_B3, "B3 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_C1] == INPUT_C1, "C1 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_C2] == INPUT_C2, "C2 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_C3] == INPUT_C3, "C3 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_D1] == INPUT_D1, "D1 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_D2] == INPUT_D2, "D2 mapping mismatch");
  static_assert(er301audio::JACK_TO_ENGINE_CHANNEL[JACK_D3] == INPUT_D3, "D3 mapping mismatch");

  static const uint32_t kButtonGpio[NUM_BUTTONS] = {
      BUTTON_MAIN1, BUTTON_MAIN2, BUTTON_MAIN3,
      BUTTON_MAIN4, BUTTON_MAIN5, BUTTON_MAIN6,
      BUTTON_DIAL1, BUTTON_DIAL2, BUTTON_DIAL3,
      BUTTON_SUB1, BUTTON_SUB2, BUTTON_SUB3,
      BUTTON_ENTER, BUTTON_UP, BUTTON_SHIFT,
      BUTTON_SELECT1, BUTTON_SELECT2, BUTTON_SELECT3, BUTTON_SELECT4};

  static const uint32_t kLedGpio[NUM_PANEL_LEDS] = {
      LED_DIAL1, LED_DIAL2, LED_IO, LED_SAFE,
      LED_OUT1, LED_OUT2, LED_OUT3, LED_OUT4,
      LED_LINK12, LED_LINK23, LED_LINK34};

  enum class LifecycleState : uint8_t
  {
    idle,
    initializing,
    running,
    stopping,
    failed
  };

  // Everything below this point is deliberately image-local. Each copied
  // plugin image receives a separate set of these globals from dyld/ld.so.
  static std::atomic<bool> g_owned{false};
  static std::atomic<LifecycleState> g_state{LifecycleState::idle};
  static std::atomic<uint32_t> g_audioCalls{0};
  alignas(64) static float g_inFrame[MAX_FRAME * NUM_INPUTS];
  alignas(64) static float g_outFrame[MAX_FRAME * NUM_OUTPUTS];

  static std::thread g_displayThread;
  static std::atomic<bool> g_displayStop{false};
  static std::mutex g_displaySleepMutex;
  static std::condition_variable g_displaySleepCondition;
  static std::mutex g_displayFrameMutex;
  static uint8_t g_displayMain[MAIN_BYTES];
  static uint8_t g_displaySub[SUB_BYTES];
  static uint64_t g_displayGeneration = 0;
  static uint64_t g_displayDeliveredGeneration = 0;

  static void resetDisplaySnapshot()
  {
    std::lock_guard<std::mutex> lock(g_displayFrameMutex);
    std::memset(g_displayMain, 0, sizeof(g_displayMain));
    std::memset(g_displaySub, 0, sizeof(g_displaySub));
    g_displayGeneration = 0;
    g_displayDeliveredGeneration = 0;
  }

  static void displayPumpLoop()
  {
    const std::chrono::microseconds period(1000000 / 55);
    const uint32_t renderTimeoutMs = 14;
    uint8_t mainRaw[MAIN_BYTES] = {};
    uint8_t subRaw[SUB_BYTES] = {};
    bool requestOutstanding = false;
    int consecutiveTimeouts = 0;
    std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now();

    while (!g_displayStop.load(std::memory_order_acquire))
    {
      next += period;
      if (!requestOutstanding)
      {
        emu::Emulator_hostRequestDisplayFrame();
        requestOutstanding = true;
      }
      if (emu::Emulator_hostWaitGetDisplayFrame(mainRaw, subRaw,
                                                renderTimeoutMs))
      {
        requestOutstanding = false;
        consecutiveTimeouts = 0;
        std::lock_guard<std::mutex> lock(g_displayFrameMutex);
        std::memcpy(g_displayMain, mainRaw, sizeof(g_displayMain));
        std::memcpy(g_displaySub, subRaw, sizeof(g_displaySub));
        ++g_displayGeneration;
      }
      else if (++consecutiveTimeouts >= 4)
      {
        requestOutstanding = false;
        consecutiveTimeouts = 0;
      }

      std::unique_lock<std::mutex> sleepLock(g_displaySleepMutex);
      if (g_displaySleepCondition.wait_until(
              sleepLock, next, []() {
                return g_displayStop.load(std::memory_order_acquire);
              }))
        break;

      const std::chrono::steady_clock::time_point now =
          std::chrono::steady_clock::now();
      if (now > next + period)
        next = now;
    }
  }

  static bool startDisplayPump()
  {
    resetDisplaySnapshot();
    g_displayStop.store(false, std::memory_order_release);
    try
    {
      g_displayThread = std::thread(displayPumpLoop);
    }
    catch (...)
    {
      g_displayStop.store(true, std::memory_order_release);
      return false;
    }
    return true;
  }

  static void stopDisplayPump()
  {
    g_displayStop.store(true, std::memory_order_release);
    g_displaySleepCondition.notify_all();
    if (g_displayThread.joinable())
      g_displayThread.join();
    resetDisplaySnapshot();
  }

  static bool backendAcquire()
  {
    bool expected = false;
    if (!g_owned.compare_exchange_strong(expected, true,
                                         std::memory_order_acq_rel))
      return false;
    if (g_state.load(std::memory_order_acquire) == LifecycleState::failed)
      g_state.store(LifecycleState::idle, std::memory_order_release);
    HostFileDialog_reset();
    HostTextDialog_reset();
    return true;
  }

  static void backendRelease()
  {
    if (!g_owned.load(std::memory_order_acquire))
      return;

    g_state.store(LifecycleState::stopping, std::memory_order_release);
    uint32_t spins = 0;
    while (g_audioCalls.load(std::memory_order_acquire) != 0)
    {
      if (spins++ < 1000)
        std::this_thread::yield();
      else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    stopDisplayPump();
    emu::Emulator_shutdownForHost();
    HostFileDialog_reset();
    HostTextDialog_reset();
    std::memset(g_inFrame, 0, sizeof(g_inFrame));
    std::memset(g_outFrame, 0, sizeof(g_outFrame));
    g_state.store(LifecycleState::idle, std::memory_order_release);
    g_owned.store(false, std::memory_order_release);
  }

  static bool backendInitialize(const RuntimePathsC *paths,
                                int nativeSampleRate)
  {
    if (!paths || !g_owned.load(std::memory_order_acquire))
      return false;

    LifecycleState expected = LifecycleState::idle;
    if (!g_state.compare_exchange_strong(expected,
                                         LifecycleState::initializing,
                                         std::memory_order_acq_rel))
      return expected == LifecycleState::running;

    if (nativeSampleRate != 48000 && nativeSampleRate != 96000)
    {
      std::fprintf(stderr,
                   "ER301: unsupported native sample rate requested: %d Hz\n",
                   nativeSampleRate);
      g_state.store(LifecycleState::failed, std::memory_order_release);
      return false;
    }

    emu::HostRuntimePaths hostPaths{
        paths->xrootPath,
        paths->rearPath,
        paths->frontPath,
        paths->configPath,
        paths->sessionPath,
        nativeSampleRate};
    if (!emu::Emulator_initializeHeadlessWithPaths(hostPaths))
    {
      g_state.store(LifecycleState::failed, std::memory_order_release);
      return false;
    }

    std::memset(g_inFrame, 0, sizeof(g_inFrame));
    std::memset(g_outFrame, 0, sizeof(g_outFrame));
    if (!startDisplayPump())
    {
      emu::Emulator_shutdownForHost();
      g_state.store(LifecycleState::failed, std::memory_order_release);
      return false;
    }
    g_state.store(LifecycleState::running, std::memory_order_release);
    return true;
  }

  static bool backendIsReady()
  {
    return g_state.load(std::memory_order_acquire) == LifecycleState::running;
  }

  static bool backendAudioRunning()
  {
    return backendIsReady() && Audio_running();
  }

  static int backendFrameLength()
  {
    const int n = FRAMELENGTH;
    return n > MAX_FRAME ? MAX_FRAME : n;
  }

  static int backendSampleRate()
  {
    return globalConfig.sampleRate;
  }

  static bool backendSavePatchState(uint32_t timeoutMs)
  {
    if (!backendIsReady() || !Audio_running())
      return false;
    return HostState_requestSave(timeoutMs);
  }

  static void backendProcessBlock(const float *inVolts, float *outVolts)
  {
    const int n = backendFrameLength();
    if (!outVolts)
      return;
    if (!inVolts || !backendIsReady())
    {
      std::memset(outVolts, 0, sizeof(float) * n * NUM_OUTPUTS);
      return;
    }

    g_audioCalls.fetch_add(1, std::memory_order_acq_rel);
    if (!backendIsReady() || !Audio_running())
    {
      g_audioCalls.fetch_sub(1, std::memory_order_acq_rel);
      std::memset(outVolts, 0, sizeof(float) * n * NUM_OUTPUTS);
      return;
    }

    for (int s = 0; s < n; ++s)
    {
      er301audio::rackInputSampleToEngine(
          inVolts + s * NUM_INPUTS,
          g_inFrame + s * NUM_INPUTS);
    }

    std::memset(g_outFrame, 0, sizeof(float) * n * NUM_OUTPUTS);
    Pump_callback(g_inFrame, g_outFrame);

    for (int s = 0; s < n; ++s)
    {
      er301audio::engineOutputSampleToRack(
          g_outFrame + s * NUM_OUTPUTS,
          outVolts + s * NUM_OUTPUTS);
    }
    g_audioCalls.fetch_sub(1, std::memory_order_acq_rel);
  }

  static bool backendGetDisplayFrame(uint8_t *mainRaw, uint8_t *subRaw)
  {
    if (!backendIsReady() || !mainRaw || !subRaw)
      return false;
    std::lock_guard<std::mutex> lock(g_displayFrameMutex);
    if (g_displayGeneration == g_displayDeliveredGeneration)
      return false;
    std::memcpy(mainRaw, g_displayMain, sizeof(g_displayMain));
    std::memcpy(subRaw, g_displaySub, sizeof(g_displaySub));
    g_displayDeliveredGeneration = g_displayGeneration;
    return true;
  }

  static void backendSetButton(int button, bool pressed)
  {
    if (backendIsReady() && button >= 0 && button < NUM_BUTTONS)
      emu::Emulator_hostSetButton(kButtonGpio[button], pressed);
  }

  static void backendTurnEncoder(int delta)
  {
    if (backendIsReady() && delta != 0)
      emu::Emulator_hostTurnEncoder(delta);
  }

  static void backendSetStorageSwitch(int pos)
  {
    if (backendIsReady())
      emu::Emulator_hostSetStorageSwitch(pos);
  }

  static void backendSetModeSwitch(int pos)
  {
    if (backendIsReady())
      emu::Emulator_hostSetModeSwitch(pos);
  }

  static bool backendPanelLed(int led)
  {
    return backendIsReady() && led >= 0 && led < NUM_PANEL_LEDS &&
           Gpio_read(kLedGpio[led]);
  }

  static void backendInputLevelLed(int index, float *red, float *green)
  {
    if (!red || !green)
      return;
    if (!backendIsReady())
    {
      *red = 0.f;
      *green = 0.f;
      return;
    }
    Pwm_hostGet(index, red, green);
  }

  static bool backendTakeHostFileDialogRequest(const char **initialPath,
                                               const char **filterSpec)
  {
    return backendIsReady() &&
           emu::HostFileDialog_takeRequest(initialPath, filterSpec);
  }

  static void backendCompleteHostFileDialog(const char *path)
  {
    if (backendIsReady())
      emu::HostFileDialog_complete(path);
  }

  static bool backendTakeHostTextDialogRequest(const char **message,
                                               const char **initialText,
                                               int *extended)
  {
    return backendIsReady() &&
           emu::HostTextDialog_takeRequest(message, initialText, extended);
  }

  static void backendCompleteHostTextDialog(const char *text)
  {
    if (backendIsReady())
      emu::HostTextDialog_complete(text);
  }

  static const void *backendImageCookie()
  {
    return static_cast<const void *>(&g_owned);
  }
}

namespace
{
  const BridgeApiV1 *localBridgeApi()
  {
    static const BridgeApiV1 api = {
        kBridgeAbiVersion,
        static_cast<uint32_t>(sizeof(BridgeApiV1)),
        er301::backendImageCookie,
        er301::backendAcquire,
        er301::backendRelease,
        er301::backendInitialize,
        er301::backendIsReady,
        er301::backendAudioRunning,
        er301::backendFrameLength,
        er301::backendSampleRate,
        er301::backendSavePatchState,
        er301::backendProcessBlock,
        er301::backendGetDisplayFrame,
        er301::backendSetButton,
        er301::backendTurnEncoder,
        er301::backendSetStorageSwitch,
        er301::backendSetModeSwitch,
        er301::backendPanelLed,
        er301::backendInputLevelLed,
        er301::backendTakeHostFileDialogRequest,
        er301::backendCompleteHostFileDialog,
        er301::backendTakeHostTextDialogRequest,
        er301::backendCompleteHostTextDialog};
    return &api;
  }
}

extern "C" ER301SoundComputer_EXPORT const BridgeApiV1 *soundcomputer_engine_api_v1()
{
  return localBridgeApi();
}

namespace er301
{
  Instance::Instance() : mSlot(nullptr)
  {
  }

  Instance::~Instance()
  {
    close();
  }

  bool Instance::open(const std::string &imageCacheRoot, std::string &error)
  {
    if (valid())
      return true;
    EngineSlot *slot = claimEngineSlot(imageCacheRoot, error);
    if (!slot)
      return false;
    mSlot.store(slot, std::memory_order_release);
    return true;
  }

  void Instance::close()
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.exchange(nullptr, std::memory_order_acq_rel));
    returnEngineSlot(slot);
  }

  bool Instance::valid() const
  {
    return mSlot.load(std::memory_order_acquire) != nullptr;
  }

  bool Instance::initialize(const RuntimePaths &paths, int nativeSampleRate)
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    if (!slot)
      return false;
    RuntimePathsC cpaths = {
        paths.xrootPath.c_str(), paths.userDataRoot.c_str(),
        paths.frontPath.c_str(), paths.rearPath.c_str(),
        paths.configPath.c_str(), paths.sessionPath.c_str(),
        paths.logPath.c_str()};
    return slot->api->initialize(&cpaths, nativeSampleRate);
  }

  bool Instance::isReady() const
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    return slot && slot->api->isReady();
  }

  bool Instance::audioRunning() const
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    return slot && slot->api->audioRunning();
  }

  int Instance::frameLength() const
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    return slot ? slot->api->frameLength() : MAX_FRAME;
  }

  int Instance::sampleRate() const
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    return slot ? slot->api->sampleRate() : 0;
  }

  bool Instance::savePatchState(uint32_t timeoutMs)
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    return slot && slot->api->savePatchState(timeoutMs);
  }

  void Instance::processBlock(const float *inVolts, float *outVolts)
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    if (slot)
      slot->api->processBlock(inVolts, outVolts);
    else if (outVolts)
      std::memset(outVolts, 0, sizeof(float) * MAX_FRAME * NUM_OUTPUTS);
  }

  bool Instance::getDisplayFrame(uint8_t *mainRaw, uint8_t *subRaw)
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    return slot && slot->api->getDisplayFrame(mainRaw, subRaw);
  }

  bool Instance::takeHostFileDialogRequest(std::string &initialPath,
                                           std::string &filterSpec)
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    if (!slot || !slot->api->takeHostFileDialogRequest)
      return false;
    const char *path = nullptr;
    const char *filter = nullptr;
    if (!slot->api->takeHostFileDialogRequest(&path, &filter))
      return false;
    initialPath = path ? path : "";
    filterSpec = filter ? filter : "";
    return true;
  }

  void Instance::completeHostFileDialog(const std::string &path)
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    if (slot && slot->api->completeHostFileDialog)
      slot->api->completeHostFileDialog(path.empty() ? nullptr : path.c_str());
  }

  bool Instance::takeHostTextDialogRequest(std::string &message,
                                           std::string &initialText,
                                           bool &extended)
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    if (!slot || !slot->api->takeHostTextDialogRequest)
      return false;

    const char *requestMessage = nullptr;
    const char *requestInitial = nullptr;
    int requestExtended = 0;
    if (!slot->api->takeHostTextDialogRequest(
            &requestMessage, &requestInitial, &requestExtended))
      return false;

    message = requestMessage ? requestMessage : "Enter text";
    initialText = requestInitial ? requestInitial : "";
    extended = requestExtended != 0;
    return true;
  }

  void Instance::completeHostTextDialog(const std::string &text,
                                        bool cancelled)
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    if (slot && slot->api->completeHostTextDialog)
      slot->api->completeHostTextDialog(cancelled ? nullptr : text.c_str());
  }

  void Instance::setButton(Button button, bool pressed)
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    if (slot)
      slot->api->setButton(static_cast<int>(button), pressed);
  }

  void Instance::turnEncoder(int delta)
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    if (slot)
      slot->api->turnEncoder(delta);
  }

  void Instance::setStorageSwitch(int pos)
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    if (slot)
      slot->api->setStorageSwitch(pos);
  }

  void Instance::setModeSwitch(int pos)
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    if (slot)
      slot->api->setModeSwitch(pos);
  }

  bool Instance::panelLed(PanelLed led) const
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    return slot && slot->api->panelLed(static_cast<int>(led));
  }

  void Instance::inputLevelLed(int index, float *red, float *green) const
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    if (slot)
      slot->api->inputLevelLed(index, red, green);
    else if (red && green)
    {
      *red = 0.f;
      *green = 0.f;
    }
  }

  unsigned Instance::slotNumber() const
  {
    EngineSlot *slot = static_cast<EngineSlot *>(
        mSlot.load(std::memory_order_acquire));
    return slot ? slot->number : 0;
  }
}
