#include "plugin.hpp"
#include "er301_bridge.hpp"
#include "er301_audio_contract.hpp"
#include "er301_sample_rate_adapter.hpp"
#include "ER301Layout.hpp"

#include <osdialog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// windows.h defines IN and OUT as empty annotation macros.
// ER-301's panel layout legitimately uses IN[] and OUT[].
#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif
#endif

namespace {

enum ER301PanelColor {
    PANEL_SILVER = 0,
    PANEL_BLACK = 1,
    NUM_PANEL_COLORS
};

enum ER301ScreenColor {
    SCREEN_AMBER = 0,
    SCREEN_TAN,
    SCREEN_CYAN,
    SCREEN_BLUE,
    SCREEN_GREEN,
    SCREEN_MINT,
    SCREEN_RED,
    SCREEN_MAGENTA,
    SCREEN_ROSE,
    SCREEN_CRYSTAL,
    NUM_SCREEN_COLORS
};

// Panel color has two layers: each Rack module keeps its own presentation
// state, while this preference is only the sticky default for newly created
// ER-301 instances. Changing one module must never recolor existing modules.
static int gPanelColorPreference = -1;

static std::string panelColorPreferencePath() {
    return asset::user("ER-301SoundComputer/panel-color.txt");
}

static int loadPanelColorPreference() {
    if (gPanelColorPreference >= 0)
        return gPanelColorPreference;

    gPanelColorPreference = PANEL_SILVER;
    std::ifstream in(panelColorPreferencePath().c_str());
    std::string value;
    if (in >> value) {
        if (value == "black")
            gPanelColorPreference = PANEL_BLACK;
        else if (value == "silver")
            gPanelColorPreference = PANEL_SILVER;
    }
    return gPanelColorPreference;
}

static void savePanelColorPreference(int color) {
    gPanelColorPreference =
        color == PANEL_BLACK ? PANEL_BLACK : PANEL_SILVER;

    const std::string root = asset::user("ER-301SoundComputer");
    system::createDirectories(root);
    std::ofstream out(panelColorPreferencePath().c_str(),
                      std::ios::out | std::ios::trunc);
    if (out)
        out << (gPanelColorPreference == PANEL_BLACK ? "black\n"
                                                       : "silver\n");
}

// Screen color uses the same per-instance + sticky-new-instance model.
// The OLED framebuffers themselves remain untouched; only their final RGB
// tint changes, preserving the ER-301's native 4-bit brightness hierarchy.
static int gScreenColorPreference = -1;

static std::string screenColorPreferencePath() {
    return asset::user("ER-301SoundComputer/screen-color.txt");
}

static const char* screenColorPreferenceName(int color) {
    switch (color) {
    case SCREEN_TAN:     return "tan";
    case SCREEN_CYAN:    return "cyan";
    case SCREEN_BLUE:    return "blue";
    case SCREEN_GREEN:   return "green";
    case SCREEN_MINT:    return "mint";
    case SCREEN_RED:     return "red";
    case SCREEN_MAGENTA: return "magenta";
    case SCREEN_ROSE:    return "rose";
    case SCREEN_CRYSTAL: return "crystal";
    case SCREEN_AMBER:
    default:              return "amber";
    }
}

static int screenColorFromPreferenceName(const std::string& value) {
    if (value == "tan")          return SCREEN_TAN;
    if (value == "cyan")         return SCREEN_CYAN;
    if (value == "blue")         return SCREEN_BLUE;
    if (value == "green")        return SCREEN_GREEN;
    if (value == "mint")         return SCREEN_MINT;
    if (value == "red")          return SCREEN_RED;
    if (value == "magenta")      return SCREEN_MAGENTA;
    if (value == "rose")         return SCREEN_ROSE;
    if (value == "crystal")      return SCREEN_CRYSTAL;

    // Migrate preferences written by earlier screen-color revisions.
    // Sky Blue is now simply Blue, Forest Green is simply Green, the brief
    // Leather experiment returns to the original Tan palette, and retired
    // White/Silver selections both land on Crystal.
    if (value == "sky-blue")     return SCREEN_BLUE;
    if (value == "forest-green") return SCREEN_GREEN;
    if (value == "leather")      return SCREEN_TAN;
    if (value == "silver")       return SCREEN_CRYSTAL;
    if (value == "white")        return SCREEN_CRYSTAL;
    return SCREEN_AMBER;
}

static int loadScreenColorPreference() {
    if (gScreenColorPreference >= 0)
        return gScreenColorPreference;

    gScreenColorPreference = SCREEN_AMBER;
    std::ifstream in(screenColorPreferencePath().c_str());
    std::string value;
    if (in >> value)
        gScreenColorPreference = screenColorFromPreferenceName(value);
    return gScreenColorPreference;
}

static void saveScreenColorPreference(int color) {
    if (color < 0 || color >= NUM_SCREEN_COLORS)
        color = SCREEN_AMBER;
    gScreenColorPreference = color;

    const std::string root = asset::user("ER-301SoundComputer");
    system::createDirectories(root);
    std::ofstream out(screenColorPreferencePath().c_str(),
                      std::ios::out | std::ios::trunc);
    if (out)
        out << screenColorPreferenceName(gScreenColorPreference) << "\n";
}

static void screenColorRGB(int color, float& r, float& g, float& b) {
    // Peak RGB values for a fully-lit OLED pixel. Dimmer framebuffer values
    // scale these peaks proportionally, so menus, anti-aliasing and scope
    // traces retain their existing relative intensities. The palettes are
    // intentionally distinct display tints rather than arbitrary RGB colors.
    switch (color) {
    case SCREEN_TAN:
        // Original Tan palette: warm parchment / incandescent tone.
        r = 0.82f; g = 0.58f; b = 0.33f;
        break;
    case SCREEN_CYAN:
        // Original cyan option: bright, cool and high-contrast.
        r = 0.05f; g = 0.88f; b = 1.00f;
        break;
    case SCREEN_BLUE:
        // The former Sky Blue palette: softer than the original saturated
        // blue, with enough green to stay comfortable on a dark panel.
        r = 0.34f; g = 0.70f; b = 1.00f;
        break;
    case SCREEN_GREEN:
        // This is the former Forest Green palette, promoted to the sole green.
        r = 0.12f; g = 0.68f; b = 0.22f;
        break;
    case SCREEN_RED:
        // Original red option.
        r = 1.00f; g = 0.12f; b = 0.07f;
        break;
    case SCREEN_MAGENTA:
        r = 1.00f; g = 0.10f; b = 0.72f;
        break;
    case SCREEN_ROSE:
        r = 1.00f; g = 0.48f; b = 0.66f;
        break;
    case SCREEN_CRYSTAL:
        // Ice-white rather than flat RGB white, giving the display a luminous
        // blue-white character while retaining all framebuffer intensities.
        r = 0.78f; g = 0.95f; b = 1.00f;
        break;
    case SCREEN_MINT:
        // Light, fresh green with a cool edge; deliberately much brighter and
        // less earthy than the standard Green palette.
        r = 0.46f; g = 1.00f; b = 0.70f;
        break;
    case SCREEN_AMBER:
    default:
        // Original ER-301-style amber option.
        r = 1.00f; g = 0.44f; b = 0.00f;
        break;
    }
}

struct RuntimeCandidate {
    std::string label;
    std::string root;
    bool development;

    RuntimeCandidate(const std::string& label_, const std::string& root_,
                     bool development_)
        : label(label_), root(root_), development(development_) {}
};

static bool isUsableXroot(const std::string& path) {
    return system::isDirectory(path) &&
           system::isFile(system::join(path, "boot", "start.lua"));
}

static bool ensureDirectory(const std::string& path, std::string& error) {
    if (system::isDirectory(path))
        return true;
    if (system::exists(path)) {
        error = "Path exists but is not a directory: " + path;
        return false;
    }
    if (!system::createDirectories(path) || !system::isDirectory(path)) {
        error = "Could not create directory: " + path;
        return false;
    }
    return true;
}

// Seed a bundled package archive into the emulated front SD card exactly once.
// The marker deliberately lives outside the card itself so deleting an archive
// from Package Manager remains a user decision rather than causing it to be
// resurrected at every Rack launch.
static bool seedBundledPackageOnce(const std::string& sharedRoot,
                                   const std::string& packageDirectory,
                                   const std::string& filename,
                                   std::string& error) {
    const std::string bootstrapRoot = system::join(sharedRoot, "bootstrap");
    if (!ensureDirectory(bootstrapRoot, error))
        return false;

    const std::string marker =
        system::join(bootstrapRoot, filename + ".seeded");
    if (system::isFile(marker))
        return true;

    const std::string source =
        asset::plugin(pluginInstance, system::join("res/er301/packages", filename));
    const std::string destination = system::join(packageDirectory, filename);

    if (!system::isFile(source)) {
        error = "Bundled package archive is missing: " + source;
        return false;
    }

    if (!system::isFile(destination)) {
        std::ifstream input(source.c_str(), std::ios::binary);
        std::ofstream output(destination.c_str(),
                             std::ios::binary | std::ios::trunc);
        if (!input || !output) {
            error = "Could not seed bundled package archive: " + destination;
            return false;
        }
        output << input.rdbuf();
        output.flush();
        if (!input.good() && !input.eof()) {
            error = "Could not read bundled package archive: " + source;
            return false;
        }
        if (!output) {
            error = "Could not write bundled package archive: " + destination;
            return false;
        }
    }

    std::ofstream markerFile(marker.c_str(), std::ios::out | std::ios::trunc);
    if (!markerFile) {
        error = "Could not record bundled package bootstrap: " + marker;
        return false;
    }
    markerFile << filename << "\n";
    return true;
}

static bool removeDirectoryTree(const std::string& path,
                                std::string& error) {
#ifdef _WIN32
    if (!system::exists(path))
        return true;

    if (system::remove(path))
        return true;

    system::removeRecursively(path);

    if (!system::exists(path))
        return true;

    error = "Could not remove package repository path: " + path;
    return false;
#else
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT)
            return true;
        error = "Could not inspect package repository path: " + path;
        return false;
    }

    if (S_ISLNK(st.st_mode) || S_ISREG(st.st_mode)) {
        if (::unlink(path.c_str()) != 0) {
            error = "Could not remove package repository item: " + path;
            return false;
        }
        return true;
    }

    if (!S_ISDIR(st.st_mode)) {
        error = "Unsupported package repository item: " + path;
        return false;
    }

    DIR* directory = ::opendir(path.c_str());
    if (!directory) {
        error = "Could not open package repository directory: " + path;
        return false;
    }

    bool ok = true;

    for (;;) {
        struct dirent* entry = ::readdir(directory);
        if (!entry)
            break;

        const std::string name(entry->d_name);

        if (name == "." || name == "..")
            continue;

        if (!removeDirectoryTree(system::join(path, name), error)) {
            ok = false;
            break;
        }
    }

    ::closedir(directory);

    if (!ok)
        return false;

    if (::rmdir(path.c_str()) != 0) {
        error = "Could not remove old private package directory: " + path;
        return false;
    }

    return true;
#endif
}

#ifdef _WIN32
static std::wstring windowsWidePath(const std::string& path) {
    if (path.empty())
        return std::wstring();

    const int required =
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);

    if (required <= 0)
        return std::wstring();

    std::vector<wchar_t> buffer(static_cast<std::size_t>(required));

    if (!MultiByteToWideChar(
            CP_UTF8, 0, path.c_str(), -1,
            buffer.data(), required))
        return std::wstring();

    return std::wstring(buffer.data());
}

static bool copyDirectoryTreeWindows(const std::string& source,
                                     const std::string& destination,
                                     std::string& error) {
    if (!ensureDirectory(destination, error))
        return false;

    std::wstring sourceWide = windowsWidePath(source);
    if (sourceWide.empty()) {
        error = "Could not convert Windows source path: " + source;
        return false;
    }

    if (!sourceWide.empty() &&
        sourceWide.back() != L'\\' &&
        sourceWide.back() != L'/')
        sourceWide += L'\\';

    const std::wstring pattern = sourceWide + L"*";

    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);

    if (find == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();

        // Empty source directory.
        if (code == ERROR_FILE_NOT_FOUND)
            return true;

        error = "Could not enumerate Windows runtime directory " +
                source + " (Windows error " +
                std::to_string(static_cast<unsigned long>(code)) + ").";
        return false;
    }

    bool ok = true;

    do {
        const std::wstring name(data.cFileName);

        if (name == L"." || name == L"..")
            continue;

        const int utf8Length =
            WideCharToMultiByte(
                CP_UTF8, 0, name.c_str(), -1,
                nullptr, 0, nullptr, nullptr);

        if (utf8Length <= 0) {
            error = "Could not convert Windows runtime filename.";
            ok = false;
            break;
        }

        std::vector<char> utf8(static_cast<std::size_t>(utf8Length));

        WideCharToMultiByte(
            CP_UTF8, 0, name.c_str(), -1,
            utf8.data(), utf8Length, nullptr, nullptr);

        const std::string itemName(utf8.data());
        const std::string sourceItem =
            system::join(source, itemName);
        const std::string destinationItem =
            system::join(destination, itemName);

        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!copyDirectoryTreeWindows(
                    sourceItem, destinationItem, error)) {
                ok = false;
                break;
            }
        }
        else {
            const std::wstring sourceItemWide =
                windowsWidePath(sourceItem);
            const std::wstring destinationItemWide =
                windowsWidePath(destinationItem);

            if (sourceItemWide.empty() ||
                destinationItemWide.empty() ||
                !CopyFileW(
                    sourceItemWide.c_str(),
                    destinationItemWide.c_str(),
                    FALSE)) {
                const DWORD code = GetLastError();

                error = "Could not copy Windows runtime file " +
                        sourceItem + " -> " + destinationItem +
                        " (Windows error " +
                        std::to_string(
                            static_cast<unsigned long>(code)) +
                        ").";
                ok = false;
                break;
            }
        }
    } while (FindNextFileW(find, &data));

    if (ok) {
        const DWORD code = GetLastError();

        if (code != ERROR_NO_MORE_FILES) {
            error = "Could not finish enumerating Windows runtime directory " +
                    source + " (Windows error " +
                    std::to_string(static_cast<unsigned long>(code)) + ").";
            ok = false;
        }
    }

    FindClose(find);
    return ok;
}
#endif

static bool replaceWithDirectorySymlink(const std::string& linkPath,
                                        const std::string& targetPath,
                                        std::string& error) {
#ifdef _WIN32
    if (system::exists(linkPath)) {
        if (!system::remove(linkPath)) {
            if (!removeDirectoryTree(linkPath, error))
                return false;
        }
    }

    const std::string parent = system::getDirectory(linkPath);
    if (!parent.empty() && !ensureDirectory(parent, error))
        return false;

    // Normal Windows users cannot be assumed to have symbolic-link
    // privileges or Developer Mode. Mirror the shared package directory
    // into this instance instead.
    if (!copyDirectoryTreeWindows(targetPath, linkPath, error))
        return false;

    return true;
#else
    struct stat st{};

    if (::lstat(linkPath.c_str(), &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            char target[4096];

            const ssize_t length =
                ::readlink(linkPath.c_str(), target, sizeof(target) - 1);

            if (length >= 0) {
                target[length] = '\0';

                if (targetPath == target)
                    return true;
            }

            if (::unlink(linkPath.c_str()) != 0) {
                error = "Could not replace stale runtime link: " + linkPath;
                return false;
            }
        }
        else {
            if (!removeDirectoryTree(linkPath, error))
                return false;
        }
    }
    else if (errno != ENOENT) {
        error = "Could not inspect runtime link: " + linkPath;
        return false;
    }

    const std::size_t slash = linkPath.find_last_of('/');

    if (slash != std::string::npos &&
        !ensureDirectory(linkPath.substr(0, slash), error))
        return false;

    if (::symlink(targetPath.c_str(), linkPath.c_str()) != 0) {
        error = "Could not create runtime link " + linkPath + " -> " +
                targetPath + ": " + std::strerror(errno);
        return false;
    }

    return true;
#endif
}

static bool replaceWithFileSymlink(const std::string& linkPath,
                                   const std::string& targetPath,
                                   std::string& error) {
#ifdef _WIN32
    if (system::exists(linkPath)) {
        if (!system::remove(linkPath)) {
            if (!removeDirectoryTree(linkPath, error))
                return false;
        }
    }

    const std::string parent = system::getDirectory(linkPath);
    if (!parent.empty() && !ensureDirectory(parent, error))
        return false;

    // Same rule for files: no Windows symlink privilege required.
    // If the shared database does not exist yet, create an empty instance
    // database and let the ER-301 package system populate it.
    if (system::isFile(targetPath)) {
        const std::wstring sourceWide = windowsWidePath(targetPath);
        const std::wstring destinationWide = windowsWidePath(linkPath);

        if (sourceWide.empty() ||
            destinationWide.empty() ||
            !CopyFileW(
                sourceWide.c_str(),
                destinationWide.c_str(),
                FALSE)) {
            const DWORD code = GetLastError();

            error = "Could not copy Windows runtime file " +
                    targetPath + " -> " + linkPath +
                    " (Windows error " +
                    std::to_string(
                        static_cast<unsigned long>(code)) +
                    ").";
            return false;
        }
    }
    else {
        std::ofstream output(linkPath.c_str(), std::ios::binary);
        if (!output) {
            error = "Could not create Windows runtime file: " + linkPath;
            return false;
        }
    }

    return true;
#else
    struct stat st{};

    if (::lstat(linkPath.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            if (!removeDirectoryTree(linkPath, error))
                return false;
        }
        else if (::unlink(linkPath.c_str()) != 0) {
            error = "Could not replace stale runtime file link: " + linkPath;
            return false;
        }
    }
    else if (errno != ENOENT) {
        error = "Could not inspect runtime file link: " + linkPath;
        return false;
    }

    const std::size_t slash = linkPath.find_last_of('/');

    if (slash != std::string::npos &&
        !ensureDirectory(linkPath.substr(0, slash), error))
        return false;

    if (::symlink(targetPath.c_str(), linkPath.c_str()) != 0) {
        error = "Could not create runtime file link " + linkPath + " -> " +
                targetPath + ": " + std::strerror(errno);
        return false;
    }

    return true;
#endif
}

static bool fileContainsText(const std::string& path,
                             const std::string& needle) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
        return false;
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str().find(needle) != std::string::npos;
}

static bool prepareSharedInstalledPackages(const std::string& sharedRoot,
                                           const std::string& instanceRear,
                                           std::string& error) {
    static std::mutex installedMutex;
    std::lock_guard<std::mutex> installedLock(installedMutex);

    // The ER-301 package ABI follows the firmware major/minor version.
    const std::string version = "v0.7";
    const std::string sharedVersion =
        system::join(sharedRoot, "rear", version);
    const std::string sharedLibs =
        system::join(sharedVersion, "libs");
    const std::string sharedMeta =
        system::join(sharedVersion, "meta");
    const std::string sharedPackageConfigs =
        system::join(sharedMeta, "packages");
    const std::string sharedInstalledDb =
        system::join(sharedMeta, "packages.db");

    const std::vector<std::string> sharedDirectories = {
        system::join(sharedRoot, "rear"),
        sharedVersion,
        sharedLibs,
        sharedMeta,
        sharedPackageConfigs};
    for (const std::string& directory : sharedDirectories) {
        if (!ensureDirectory(directory, error))
            return false;
    }

    // Older development builds unpacked Core directly into the shared runtime.
    // The release layout keeps Core as a .pkg archive until the user installs it.
    // Remove only that known stale preinstallation when no global package database
    // records Core as installed.
    const std::string staleCore = system::join(sharedLibs, "core");
    const bool coreRecordedInstalled =
        fileContainsText(sharedInstalledDb, "core-");
    if (!coreRecordedInstalled && system::exists(staleCore) &&
        !removeDirectoryTree(staleCore, error))
        return false;

    const std::string instanceVersion =
        system::join(instanceRear, version);
    const std::string instanceMeta =
        system::join(instanceVersion, "meta");
    if (!ensureDirectory(instanceVersion, error) ||
        !ensureDirectory(instanceMeta, error))
        return false;

    // Installed packages and their installed-state database are global. Each
    // engine still receives its own Lua state and its own prepared native
    // image, but archives and extracted package assets exist only once.
    if (!replaceWithDirectorySymlink(
            system::join(instanceVersion, "libs"), sharedLibs, error))
        return false;
    if (!replaceWithDirectorySymlink(
            system::join(instanceMeta, "packages"),
            sharedPackageConfigs, error))
        return false;
    if (!replaceWithFileSymlink(
            system::join(instanceMeta, "packages.db"),
            sharedInstalledDb, error))
        return false;

    const std::string firmwareConfig =
        system::join(instanceRear, "firmware.cfg");
    if (!system::isFile(firmwareConfig)) {
        std::ofstream output(firmwareConfig.c_str(), std::ios::trunc);
        if (!output) {
            error = "Could not create instance firmware.cfg: " +
                    firmwareConfig;
            return false;
        }
        output << "SAMPLERATE 48000\nFRAMELENGTH 128\n";
    }
    return true;
}

static std::string makeInstanceKey() {
    static std::atomic<unsigned long long> serial{0};
    const unsigned long long count = serial.fetch_add(1) + 1;
    const unsigned long long ticks = static_cast<unsigned long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::ostringstream stream;
    stream << std::hex << ticks << "-" << count;
    return stream.str();
}

static std::mutex& liveInstanceKeyMutex() {
    static std::mutex mutex;
    return mutex;
}

static std::set<std::string>& liveInstanceKeys() {
    static std::set<std::string> keys;
    return keys;
}

// Rack's Duplicate action copies module JSON verbatim. Without a live-key
// registry, the duplicate would inherit the same private rear/config roots
// and both engines would race over patch hand-offs and settings. The shared
// front card is intentional. The first live module keeps the persisted key;
// later duplicates
// receive a fresh key that will be written on the next Rack save.
static bool claimLiveInstanceKey(std::string& key) {
    std::lock_guard<std::mutex> lock(liveInstanceKeyMutex());
    std::set<std::string>& keys = liveInstanceKeys();
    if (!key.empty() && keys.insert(key).second)
        return false;

    do {
        key = makeInstanceKey();
    } while (!keys.insert(key).second);
    return true;
}

static void releaseLiveInstanceKey(const std::string& key) {
    if (key.empty())
        return;
    std::lock_guard<std::mutex> lock(liveInstanceKeyMutex());
    liveInstanceKeys().erase(key);
}

static bool resolveRuntimePaths(er301::RuntimePaths& paths,
                                const std::string& instanceKey,
                                std::string& error) {
    // The front card is one physical resource on the hardware ER-301. Model
    // it the same way here: every ER-301 Sound Computer instance sees the exact same front
    // filesystem for samples, packages, quicksaves, presets, recordings and
    // error logs. Engine/rear/config/session state remains isolated per Rack
    // module so multiple ER-301s can still run independently.
    const std::string sharedRoot = asset::user("ER-301SoundComputer");
    paths.userDataRoot = system::join(sharedRoot, "instances", instanceKey);
    paths.frontPath = system::join(sharedRoot, "front");
    paths.rearPath = system::join(paths.userDataRoot, "rear");
    paths.configPath = system::join(paths.userDataRoot, "config", "emu.config");
    paths.sessionPath = system::join(paths.userDataRoot, "config", "emu.session");
    paths.logPath = system::join(paths.frontPath, "ER-301", "logs");
    paths.engineImageCacheRoot = system::join(sharedRoot, "engine-images");

    const std::string frontCardRoot =
        system::join(paths.frontPath, "ER-301");
    const std::string packageDirectory =
        system::join(frontCardRoot, "packages");
    const std::vector<std::string> requiredDirectories = {
        sharedRoot,
        system::join(sharedRoot, "instances"),
        paths.engineImageCacheRoot,
        paths.userDataRoot,
        paths.rearPath,
        system::join(paths.userDataRoot, "config"),
        paths.frontPath,
        frontCardRoot,
        packageDirectory,
        paths.logPath};
    for (const std::string& directory : requiredDirectories) {
        if (!ensureDirectory(directory, error))
            return false;
    }

    // Core is part of the normal ER-301 user-space package set rather than a
    // hidden VCV-only library. Ship its original archive on the emulated front
    // card with the exact upstream filename; Package Manager performs the
    // actual one-time installation so it can still be uninstalled normally.
    if (!seedBundledPackageOnce(sharedRoot, packageDirectory,
                                "core-0.7.0-dev1.8.pkg", error))
        return false;

    if (!prepareSharedInstalledPackages(sharedRoot, paths.rearPath, error))
        return false;

    paths.bundledRuntimeRoot = asset::plugin(pluginInstance, "res/er301");

    std::vector<RuntimeCandidate> candidates;
    candidates.push_back({"packaged plugin runtime",
                          paths.bundledRuntimeRoot, false});
    candidates.push_back({"Rack user development runtime",
                          system::join(sharedRoot, "runtime"), true});

    if (const char* envRoot = std::getenv("ER301SoundComputer_RUNTIME_ROOT")) {
        if (envRoot[0])
            candidates.push_back({"ER301SoundComputer_RUNTIME_ROOT", envRoot, true});
    }

    const std::string cwd = system::getWorkingDirectory();
    candidates.push_back({"adjacent source tree from working directory",
                          system::join(cwd, "..", "er-301-native"), true});
    candidates.push_back({"source tree below working directory",
                          system::join(cwd, "er-301-native"), true});
    const std::string pluginRoot = asset::plugin(pluginInstance);
    candidates.push_back({"adjacent source tree from plugin directory",
                          system::join(pluginRoot, "..", "er-301-native"), true});

    std::string checked;
    for (const RuntimeCandidate& candidate : candidates) {
        const std::string direct = candidate.root;
        const std::string nested = system::join(candidate.root, "xroot");
        std::string selected;
        if (isUsableXroot(nested))
            selected = nested;
        else if (isUsableXroot(direct))
            selected = direct;

        if (!checked.empty())
            checked += "; ";
        checked += candidate.label + "=" + candidate.root;

        if (!selected.empty()) {
            paths.xrootPath = selected;
            paths.usingDevelopmentFallback = candidate.development;
            if (candidate.development)
                paths.developmentRuntimeRoot = candidate.root;
            return true;
        }
    }

    error = "No valid ER-301 xroot was found. Checked: " + checked +
            ". Run `make runtime` for the development setup or provide "
            "ER301SoundComputer_RUNTIME_ROOT.";
    return false;
}

static const char* kPatchStateFilename = "er301-state.save";
static const char* kRuntimeSnapshotFilename = "rack-current.save";
static const char* kRuntimeRestoreFilename = "rack-restore.save";

static bool copyFileAtomically(const std::string& source,
                               const std::string& destination,
                               std::string& error) {
    if (!system::isFile(source)) {
        error = "Source snapshot does not exist: " + source;
        return false;
    }

    const std::string temporary = destination + ".tmp";
    if (system::exists(temporary))
        system::remove(temporary);
    if (!system::copy(source, temporary) || !system::isFile(temporary)) {
        error = "Could not copy snapshot to temporary file: " + temporary;
        system::remove(temporary);
        return false;
    }

    // Rack's cross-platform rename overwrites an existing file. Keep the
    // previous snapshot intact until the fully written temporary file can be
    // atomically installed.
    if (!system::rename(temporary, destination)) {
        error = "Could not finalize snapshot: " + destination;
        system::remove(temporary);
        return false;
    }
    return true;
}

static const size_t kMaxInlineStateBytes = 64u * 1024u * 1024u;

static bool readBinaryFile(const std::string& path,
                           std::vector<uint8_t>& data,
                           std::string& error) {
    std::ifstream input(path.c_str(), std::ios::binary | std::ios::ate);
    if (!input) {
        error = "Could not open state file: " + path;
        return false;
    }

    const std::streamoff size = input.tellg();
    if (size < 0 || static_cast<unsigned long long>(size) >
                        static_cast<unsigned long long>(kMaxInlineStateBytes)) {
        error = "State file has an invalid or unsupported size: " + path;
        return false;
    }

    data.resize(static_cast<size_t>(size));
    input.seekg(0, std::ios::beg);
    if (!data.empty())
        input.read(reinterpret_cast<char*>(data.data()), size);
    if (!input && !data.empty()) {
        error = "Could not read state file: " + path;
        data.clear();
        return false;
    }
    return true;
}

static bool writeBinaryFileAtomically(const std::vector<uint8_t>& data,
                                      const std::string& destination,
                                      std::string& error) {
    if (data.size() > kMaxInlineStateBytes) {
        error = "State snapshot exceeds the inline preset size limit.";
        return false;
    }

    const std::string temporary = destination + ".tmp";
    if (system::exists(temporary))
        system::remove(temporary);

    {
        std::ofstream output(temporary.c_str(),
                             std::ios::binary | std::ios::out |
                             std::ios::trunc);
        if (!output) {
            error = "Could not create temporary state file: " + temporary;
            return false;
        }
        if (!data.empty()) {
            output.write(reinterpret_cast<const char*>(data.data()),
                         static_cast<std::streamsize>(data.size()));
        }
        output.close();
        if (!output) {
            error = "Could not write temporary state file: " + temporary;
            system::remove(temporary);
            return false;
        }
    }

    if (!system::rename(temporary, destination)) {
        error = "Could not finalize state file: " + destination;
        system::remove(temporary);
        return false;
    }
    return true;
}

} // namespace

/**
 * ER301 — the ER-301 Sound Computer as a native VCV Rack module.
 *
 * - Headless engine: no SDL window, no SDL audio device, no SDL event loop.
 *   Rack owns the audio clock (process()), the UI thread (widget step()),
 *   and the module lifecycle.
 * - The live ER-301 displays are drawn into the panel with NanoVG from the
 *   engine's real framebuffers (raw formats from hal/display.h, decoded
 *   exactly like the emulator's Window::renderMainFrame/renderSubFrame).
 * - 20 inputs / 4 outputs are real Rack ports bridged into the engine's
 *   Pump graph. Voltage scaling: 1.0 engine unit == 10 V (see bridge).
 * - Multiple active modules are supported. Each one claims a separately
 *   loaded copy of the singleton-heavy engine image and private rear/config roots.
 * - Native ER-301 engine modes at 48 kHz and 96 kHz. Rack rates that do not
 *   match the selected native mode are converted with Rack's highest-quality
 *   Speex resampler without changing ER-301 Sound Computer's existing voltage contract.
 *
 * Realtime notes for process(): no allocation, no files, no logging, no
 * blocking. The per-block engine render enters through Pump_callback(),
 * the same entry the hardware audio task uses; graph edits arrive via its
 * lock-free connection queue.
 */
struct ER301 : Module {
    enum ParamId {
        ENUMS(BUTTON_PARAMS, er301::NUM_BUTTONS),
        ENUMS(LINK_PARAMS, 3), // momentary SELECT1+2, SELECT2+3, SELECT3+4 chords
        STORAGE_PARAM, // 0=eject 1=admin 2=user
        MODE_PARAM,    // 0=scope 1=edit 2=hold
        PARAMS_LEN
    };

    enum InputId {
        ENUMS(JACK_INPUTS, er301::NUM_INPUT_JACKS),
        INPUTS_LEN
    };

    enum OutputId {
        OUT1_OUTPUT, OUT2_OUTPUT, OUT3_OUTPUT, OUT4_OUTPUT,
        OUTPUTS_LEN
    };

    enum LightId {
        ENUMS(PANEL_LIGHTS, er301::NUM_PANEL_LEDS),
        ENUMS(ABCD_LIGHTS, 12 * 2), // GreenRed pairs: A1,B1,C1,D1,A2,...
        LIGHTS_LEN
    };

    enum ScrollRegionMode {
        SCROLL_ENCODER_ONLY = 0,
        SCROLL_CONTROLS = 1,
        SCROLL_MOST_PANEL = 2,
        NUM_SCROLL_REGION_MODES
    };

    // Mouse-wheel/trackpad routing and presentation colors are per-instance
    // UI state saved with the Rack patch. A value of -1 means a newly created
    // module has not yet adopted the current sticky color default.
    int scrollRegionMode = SCROLL_CONTROLS;
    int panelColor = -1;
    int screenColor = -1;

    // --- lifecycle state (UI thread) ---
    er301::Instance engine;
    std::string instanceKey = makeInstanceKey();
    bool instanceKeyClaimed = false;
    bool initTried = false;
    std::atomic<bool> initFailed{false};
    bool togglesApplied = false;

    // --- Rack patch storage / runtime state (non-audio threads) ---
    er301::RuntimePaths runtimePaths;
    bool runtimePathsReady = false;
    bool patchRestorePrepared = false;

    // Rack patch saves use per-module patch storage so .vcv JSON stays small.
    // Rack module presets/clipboard JSON do not invoke onSave(), so a preset
    // carries the same ER-301 snapshot inline and can restore independently.
    std::vector<uint8_t> pendingModulePresetState;
    bool freshStateRequested = false;
    std::atomic<bool> suppressInlineStateOnce{false};

    // --- control state (UI thread) ---
    bool prevButton[er301::NUM_BUTTONS] = {};
    bool prevLink[3] = {};
    bool shiftLatched = false;
    bool shiftReleasePending = false;
    std::chrono::steady_clock::time_point shiftReleaseDeadline;
    int prevStorage = -1;
    int prevMode = -1;
    std::atomic<int> encoderDelta{0}; // written by the encoder widget

    // --- audio adapters ---
    // The direct adapter preserves one-engine-frame latency at native 48/96 kHz.
    // The rate adapter is used only when Rack and the chosen ER-301 firmware mode
    // differ. Both are fixed-storage and allocation-free from process().
    er301audio::BlockAdapter audioAdapter;
    er301audio::SampleRateAdapter sampleRateAdapter;
    std::atomic<int> rackSampleRate{48000};
    int nativeSampleRate = 0;
    std::atomic<bool> audioConfigurationReady{false};
    std::atomic<unsigned> audioConfigurationGeneration{0};
    unsigned audioConfigurationSeen = 0;
    int reportedRackSampleRate = 0;
    int reportedNativeSampleRate = 0;

    ER301() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        static const char* buttonNames[er301::NUM_BUTTONS] = {
            "M1", "M2", "M3", "M4", "M5", "M6",
            "Dial (fine/coarse)", "Cancel", "Home",
            "S1", "S2", "S3",
            "Enter", "Up", "Shift",
            "OUT1 select", "OUT2 select", "OUT3 select", "OUT4 select"};
        for (int i = 0; i < er301::NUM_BUTTONS; i++) {
            configButton(BUTTON_PARAMS + i, buttonNames[i]);
        }
        configButton(LINK_PARAMS + 0, "Link channels 1 and 2");
        configButton(LINK_PARAMS + 1, "Link channels 2 and 3");
        configButton(LINK_PARAMS + 2, "Link channels 3 and 4");
        configSwitch(STORAGE_PARAM, 0.f, 2.f, 2.f, "Storage",
                     {"eject", "admin", "user"});
        configSwitch(MODE_PARAM, 0.f, 2.f, 1.f, "Mode",
                     {"scope", "edit", "hold"});

        // ER-301 is a stateful computer, not a collection of independent Rack
        // knob values. Rack's Randomize command is intentionally inert.
        for (ParamQuantity* quantity : paramQuantities) {
            if (quantity)
                quantity->randomizeEnabled = false;
        }

        static const char* inputNames[er301::NUM_INPUT_JACKS] = {
            "G1", "G2", "G3", "G4",
            "IN1", "IN2", "IN3", "IN4",
            "A1", "A2", "A3", "B1", "B2", "B3",
            "C1", "C2", "C3", "D1", "D2", "D3"};
        for (int i = 0; i < er301::NUM_INPUT_JACKS; i++) {
            configInput(JACK_INPUTS + i, inputNames[i]);
        }
        configOutput(OUT1_OUTPUT, "OUT1");
        configOutput(OUT2_OUTPUT, "OUT2");
        configOutput(OUT3_OUTPUT, "OUT3");
        configOutput(OUT4_OUTPUT, "OUT4");

    }

    ~ER301() override {
        // The isolated engine slot closes audio admission, waits for in-flight
        // work, tears down this module's Lua/task/display state, and then
        // returns the still-loaded image to the process-wide slot pool.
        engine.close();
        if (instanceKeyClaimed) {
            releaseLiveInstanceKey(instanceKey);
            instanceKeyClaimed = false;
        }
    }

    void resetHostLifecycle(bool newIdentity) {
        engine.close();

        if (newIdentity) {
            if (instanceKeyClaimed) {
                releaseLiveInstanceKey(instanceKey);
                instanceKeyClaimed = false;
            }
            instanceKey = makeInstanceKey();
        }

        initTried = false;
        initFailed.store(false, std::memory_order_release);
        togglesApplied = false;
        runtimePaths = er301::RuntimePaths();
        runtimePathsReady = false;
        patchRestorePrepared = false;

        shiftLatched = false;
        shiftReleasePending = false;
        std::fill(prevButton, prevButton + er301::NUM_BUTTONS, false);
        std::fill(prevLink, prevLink + 3, false);
        prevStorage = -1;
        prevMode = -1;
        encoderDelta.store(0, std::memory_order_release);

        nativeSampleRate = 0;
        audioConfigurationReady.store(false, std::memory_order_release);
        audioConfigurationGeneration.fetch_add(1, std::memory_order_acq_rel);
        reportedRackSampleRate = 0;
        reportedNativeSampleRate = 0;

        for (int i = 0; i < LIGHTS_LEN; ++i)
            lights[i].setBrightness(0.f);
    }

    bool captureCurrentState(std::vector<uint8_t>& state,
                             std::string& error) {
        state.clear();
        if (!runtimePathsReady || !engine.valid() ||
            initFailed.load(std::memory_order_acquire) ||
            !engine.isReady() || !engine.audioRunning()) {
            error = "ER-301 engine is not ready to serialize module state.";
            return false;
        }

        const std::string runtimeSnapshot = system::join(
            runtimePaths.rearPath, kRuntimeSnapshotFilename);
        if (system::exists(runtimeSnapshot))
            system::remove(runtimeSnapshot);

        if (!engine.savePatchState(15000)) {
            error = "ER-301 state serializer did not complete.";
            return false;
        }
        if (!system::isFile(runtimeSnapshot)) {
            error = "ER-301 state serializer produced no snapshot.";
            return false;
        }
        return readBinaryFile(runtimeSnapshot, state, error);
    }

    bool writeStateToPatchStorage(const std::vector<uint8_t>& state,
                                  std::string& error) {
        std::string storageDirectory;
        try {
            storageDirectory = createPatchStorageDirectory();
        }
        catch (const Exception& ex) {
            error = std::string("Could not create Rack patch storage: ") +
                    ex.what();
            return false;
        }
        if (storageDirectory.empty()) {
            error = "Rack returned an empty patch storage directory.";
            return false;
        }
        return writeBinaryFileAtomically(
            state, system::join(storageDirectory, kPatchStateFilename), error);
    }

    void clearStoredPatchState() {
        try {
            const std::string storageDirectory = getPatchStorageDirectory();
            if (storageDirectory.empty())
                return;
            const std::string storedState = system::join(
                storageDirectory, kPatchStateFilename);
            if (system::isFile(storedState) && !system::remove(storedState))
                WARN("ER301: could not clear stale Rack patch state: %s",
                     storedState.c_str());
        }
        catch (const Exception&) {
            // A module without patch storage already has nothing to clear.
        }
    }

    bool preparePatchRestore(std::string& error) {
        if (patchRestorePrepared || !runtimePathsReady)
            return true;
        patchRestorePrepared = true;

        // Always remove a stale hand-off left by an interrupted prior boot.
        // A Rack patch without module storage must start from the ER-301's
        // normal default/session behavior, never another patch's state.
        const std::string restorePath = system::join(
            runtimePaths.rearPath, kRuntimeRestoreFilename);
        if (system::exists(restorePath) && !system::remove(restorePath)) {
            error = "Could not remove stale Rack restore hand-off: " +
                    restorePath;
            return false;
        }

        // Initialize intentionally starts this one module from a clean ER-301
        // state. Never let an older Rack patch-storage snapshot leak back in.
        if (freshStateRequested)
            return true;

        // Preset/Open/Paste state is self-contained in module JSON. Stage it
        // through the same rack-restore.save path used by normal patch recall.
        if (!pendingModulePresetState.empty()) {
            if (!writeBinaryFileAtomically(
                    pendingModulePresetState, restorePath, error))
                return false;
            INFO("ER301: staged inline module-preset state for restore (%llu bytes).",
                 (unsigned long long)pendingModulePresetState.size());
            return true;
        }

        std::string storageDirectory;
        try {
            storageDirectory = getPatchStorageDirectory();
        }
        catch (const Exception& e) {
            error = std::string("Could not inspect Rack patch storage: ") +
                    e.what();
            return false;
        }
        if (storageDirectory.empty())
            return true;

        const std::string storedState = system::join(
            storageDirectory, kPatchStateFilename);
        if (!system::isFile(storedState))
            return true;

        if (!copyFileAtomically(storedState, restorePath, error))
            return false;

        INFO("ER301: staged Rack patch state for restore: %s",
             storedState.c_str());
        return true;
    }

    void onSave(const SaveEvent& e) override {
        (void)e;

        // Rack calls onSave() before serializing a .vcv patch (and before
        // Duplicate), but not for module presets/clipboard JSON. Consume this
        // flag in dataToJson() to keep the large ER-301 snapshot out of normal
        // patch JSON while still embedding it in .vcvm/clipboard presets.
        suppressInlineStateOnce.store(true, std::memory_order_release);

        // A preset can be loaded and the patch saved before the next UI step
        // has rebooted the isolated engine. Preserve that pending state directly
        // in Rack's patch storage rather than falling back to an older snapshot.
        if (!pendingModulePresetState.empty()) {
            std::string error;
            if (!writeStateToPatchStorage(pendingModulePresetState, error)) {
                WARN("ER301: could not save pending module-preset state with "
                     "Rack patch: %s", error.c_str());
            }
            return;
        }

        // Initialize is also undoable/saveable before the next UI step. Remove
        // the previous stored snapshot so a later patch load remains fresh.
        if (freshStateRequested) {
            clearStoredPatchState();
            return;
        }

        std::vector<uint8_t> state;
        std::string error;
        if (!captureCurrentState(state, error)) {
            // Do not erase a previously valid patch snapshot merely because
            // Rack autosaved while the ER-301 was still booting or inactive.
            return;
        }

        if (writeStateToPatchStorage(state, error)) {
            INFO("ER301: saved internal state with Rack patch (%llu bytes).",
                 (unsigned long long)state.size());
        }
        else {
            WARN("ER301: could not save state into Rack patch storage: %s",
                 error.c_str());
        }
    }

    void onReset(const ResetEvent& e) override {
        // Reset Rack-facing controls first, then make this module equivalent to
        // deleting it and adding a fresh ER-301 instance. Shared front-card
        // samples/packages remain shared exactly as they are for a new module.
        Module::onReset(e);
        pendingModulePresetState.clear();
        freshStateRequested = true;
        scrollRegionMode = SCROLL_CONTROLS;
        panelColor = loadPanelColorPreference();
        screenColor = loadScreenColorPreference();
        resetHostLifecycle(true);
    }

    void onRandomize(const RandomizeEvent& e) override {
        (void)e;
        // Intentionally disabled. Randomizing the ER-301's computer state (or
        // its hardware-style momentary controls) has no useful semantics.
    }

    bool desiredButtonState(int i) {
        bool down = params[BUTTON_PARAMS + i].getValue() > 0.5f;
        if (i == er301::BTN_SHIFT)
            down = down || shiftLatched;
        if (i >= er301::BTN_SELECT1 && i <= er301::BTN_SELECT4) {
            int channel = i - er301::BTN_SELECT1;
            if (channel > 0)
                down = down || params[LINK_PARAMS + channel - 1].getValue() > 0.5f;
            if (channel < 3)
                down = down || params[LINK_PARAMS + channel].getValue() > 0.5f;
        }
        return down;
    }

    void syncButtonsNow() {
        if (!engine.valid() || !engine.isReady())
            return;
        for (int i = 0; i < er301::NUM_BUTTONS; i++) {
            bool down = desiredButtonState(i);
            if (down != prevButton[i]) {
                prevButton[i] = down;
                engine.setButton((er301::Button)i, down);
            }
        }
    }

    void setShiftLatched(bool latched) {
        shiftLatched = latched;
        shiftReleasePending = false;
        params[BUTTON_PARAMS + er301::BTN_SHIFT].setValue(latched ? 1.f : 0.f);
        syncButtonsNow();
    }

    void toggleShiftLatch() {
        setShiftLatched(!shiftLatched);
    }

    // Keep SHIFT electrically down for a short grace period after the target
    // control is released. The ER-301 scans the GPIO state on its own UI
    // thread, so releasing SHIFT in the same Rack callback as S1/S2/S3 can make
    // the firmware observe the unshifted command. The delay lets the complete
    // target press/release edge be consumed before SHIFT returns high.
    void finishLatchedShiftAction() {
        if (!shiftLatched)
            return;
        shiftReleasePending = true;
        shiftReleaseDeadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(100);
    }

    void serviceShiftLatch() {
        if (shiftLatched && shiftReleasePending &&
            std::chrono::steady_clock::now() >= shiftReleaseDeadline) {
            setShiftLatched(false);
        }
    }

    void turnEncoderNow(int delta) {
        if (delta == 0)
            return;
        if (engine.valid() && engine.isReady())
            engine.turnEncoder(delta);
        else
            encoderDelta.fetch_add(delta);
        // A latched SHIFT applies to one encoder gesture, then releases.
        finishLatchedShiftAction();
    }

    void configureAudioPath(int newRackSampleRate) {
        const int sanitizedRackRate = std::max(8000,
            std::min(newRackSampleRate, 768000));
        rackSampleRate.store(sanitizedRackRate, std::memory_order_release);
        audioConfigurationReady.store(false, std::memory_order_release);
        if (nativeSampleRate == 48000 || nativeSampleRate == 96000) {
            sampleRateAdapter.configure(sanitizedRackRate, nativeSampleRate,
                                        engine.frameLength());
            audioConfigurationGeneration.fetch_add(1,
                std::memory_order_acq_rel);
            audioConfigurationReady.store(true, std::memory_order_release);
        }
    }

    // Opens Rack's cross-platform native file chooser on the Rack UI thread.
    // The ER-301 Lua UI runs in an isolated engine thread, so it only posts a
    // request through the bridge and polls for the result.
    void serviceHostFileDialog() {
        std::string initialPath;
        std::string filterSpec;
        if (!engine.takeHostFileDialogRequest(initialPath, filterSpec))
            return;

        osdialog_filters* filters = filterSpec.empty()
            ? nullptr
            : osdialog_filters_parse(filterSpec.c_str());
        char* selected = osdialog_file(
            OSDIALOG_OPEN,
            initialPath.empty() ? nullptr : initialPath.c_str(),
            nullptr,
            filters);
        if (filters)
            osdialog_filters_free(filters);

        std::string result;
        if (selected) {
            result = selected;
            std::free(selected);
#ifdef _WIN32
            // The ER-301 Path module intentionally uses '/' on all hosts.
            std::replace(result.begin(), result.end(), '\\', '/');
#endif
        }
        engine.completeHostFileDialog(result);
    }

    // Called from ER301Widget::step() on the Rack UI thread.
    void uiStep() {
        if (!initTried) {
            initTried = true;
            if (!instanceKeyClaimed) {
                const std::string requestedKey = instanceKey;
                const bool regeneratedKey = claimLiveInstanceKey(instanceKey);
                instanceKeyClaimed = true;
                if (regeneratedKey) {
                    WARN("ER301: duplicate/invalid instance key '%s'; assigned '%s' "
                         "to keep writable state isolated.",
                         requestedKey.c_str(), instanceKey.c_str());
                }
            }

            std::string runtimeError;
            if (!resolveRuntimePaths(runtimePaths, instanceKey, runtimeError)) {
                initFailed.store(true, std::memory_order_release);
                WARN("ER301: instance runtime initialization failed: %s",
                     runtimeError.c_str());
                return;
            }
            runtimePathsReady = true;

            std::string restoreError;
            if (!preparePatchRestore(restoreError)) {
                initFailed.store(true, std::memory_order_release);
                WARN("ER301: Rack patch restore preparation failed: %s",
                     restoreError.c_str());
                return;
            }

            std::string engineError;
            if (!engine.open(runtimePaths.engineImageCacheRoot, engineError)) {
                initFailed.store(true, std::memory_order_release);
                WARN("ER301: could not claim an isolated engine image: %s",
                     engineError.c_str());
                return;
            }

            int hostRate = rackSampleRate.load(std::memory_order_acquire);
            if (APP && APP->engine)
                hostRate = (int)std::lround(APP->engine->getSampleRate());
            rackSampleRate.store(hostRate, std::memory_order_release);
            nativeSampleRate = er301audio::chooseNativeSampleRate(hostRate);

            if (!engine.initialize(runtimePaths, nativeSampleRate)) {
                initFailed.store(true, std::memory_order_release);
                WARN("ER301: isolated engine initialization failed; see log.");
                engine.close();
                return;
            }

            // The hand-off has now been consumed by the ER-301 boot path.
            pendingModulePresetState.clear();
            freshStateRequested = false;

            configureAudioPath(hostRate);
            INFO("ER301[%s]: engine slot %u initialized.",
                 instanceKey.c_str(), engine.slotNumber());
            INFO("ER301[%s]: xroot: %s%s", instanceKey.c_str(),
                 runtimePaths.xrootPath.c_str(),
                 runtimePaths.usingDevelopmentFallback
                     ? " (development fallback)" : " (packaged)");
            INFO("ER301[%s]: shared front card: %s",
                 instanceKey.c_str(), runtimePaths.frontPath.c_str());
            INFO("ER301[%s]: private engine data: %s",
                 instanceKey.c_str(), runtimePaths.userDataRoot.c_str());
            INFO("ER301[%s]: engine initialized (rate=%d Hz, frame=%d).",
                 instanceKey.c_str(), engine.sampleRate(),
                 engine.frameLength());
            return;
        }

        if (initFailed.load(std::memory_order_acquire) || !engine.isReady())
            return;

        serviceHostFileDialog();

        const int currentRackRate = rackSampleRate.load(
            std::memory_order_acquire);
        if (audioConfigurationReady.load(std::memory_order_acquire) &&
            (currentRackRate != reportedRackSampleRate ||
             nativeSampleRate != reportedNativeSampleRate)) {
            reportedRackSampleRate = currentRackRate;
            reportedNativeSampleRate = nativeSampleRate;
            if (currentRackRate == nativeSampleRate) {
                INFO("ER301[%s]: native audio path at %d Hz.",
                     instanceKey.c_str(), nativeSampleRate);
            }
            else {
                INFO("ER301[%s]: sample-rate conversion active: Rack %d Hz "
                     "<-> ER-301 %d Hz (quality 10).",
                     instanceKey.c_str(), currentRackRate, nativeSampleRate);
            }
        }

        syncButtonsNow();
        serviceShiftLatch();
        for (int i = 0; i < 3; i++)
            prevLink[i] = params[LINK_PARAMS + i].getValue() > 0.5f;

        const int storage = (int)std::round(
            params[STORAGE_PARAM].getValue());
        const int mode = (int)std::round(params[MODE_PARAM].getValue());
        if (!togglesApplied || storage != prevStorage) {
            prevStorage = storage;
            engine.setStorageSwitch(3 - storage);
        }
        if (!togglesApplied || mode != prevMode) {
            prevMode = mode;
            engine.setModeSwitch(3 - mode);
        }
        togglesApplied = true;

        const int delta = encoderDelta.exchange(0);
        if (delta != 0)
            engine.turnEncoder(delta);

        for (int i = 0; i < er301::NUM_PANEL_LEDS; i++) {
            lights[PANEL_LIGHTS + i].setBrightness(
                engine.panelLed((er301::PanelLed)i) ? 1.f : 0.f);
        }
        for (int i = 0; i < 12; i++) {
            float red = 0.f, green = 0.f;
            engine.inputLevelLed(i, &red, &green);
            lights[ABCD_LIGHTS + 2 * i + 0].setBrightness(green);
            lights[ABCD_LIGHTS + 2 * i + 1].setBrightness(red);
        }
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        const int newRate = (int)std::lround(e.sampleRate);
        rackSampleRate.store(newRate, std::memory_order_release);

        // Rack calls this event when the module enters the engine and whenever
        // its sample rate changes. Before ER-301 boot it only records the rate
        // used to select 48/96 kHz. After boot the event is serialized with
        // process(), so rebuilding Speex state here cannot race the audio path.
        if (engine.valid() &&
            !initFailed.load(std::memory_order_acquire) &&
            engine.isReady() &&
            (nativeSampleRate == 48000 || nativeSampleRate == 96000)) {
            configureAudioPath(newRate);
        }
    }

    void process(const ProcessArgs& args) override {
        (void)args;
        const bool ready = engine.valid() &&
            !initFailed.load(std::memory_order_acquire) && engine.isReady() &&
            audioConfigurationReady.load(std::memory_order_acquire);

        if (!ready) {
            for (int c = 0; c < er301::NUM_OUTPUTS; ++c)
                outputs[OUT1_OUTPUT + c].setVoltage(0.f);
            return;
        }

        const unsigned generation = audioConfigurationGeneration.load(
            std::memory_order_acquire);
        if (generation != audioConfigurationSeen) {
            // Never resume midway through a direct-path frame after Rack's
            // rate changes. The SRC adapter was already reset by configure().
            audioAdapter.reset(engine.frameLength());
            audioConfigurationSeen = generation;
        }

        float rackInputs[er301audio::NUM_INPUTS];
        float rackOutputs[er301audio::NUM_OUTPUTS];
        for (int jack = 0; jack < er301::NUM_INPUT_JACKS; ++jack)
            rackInputs[jack] = inputs[JACK_INPUTS + jack].getVoltage();

        const int currentRackRate = rackSampleRate.load(
            std::memory_order_acquire);
        if (currentRackRate == nativeSampleRate) {
            audioAdapter.processSample(
                rackInputs, rackOutputs, engine.frameLength(),
                [this](const float* inputFrame, float* outputFrame) {
                    engine.processBlock(inputFrame, outputFrame);
                });
        }
        else {
            sampleRateAdapter.processSample(
                rackInputs, rackOutputs,
                [this](const float* inputFrame, float* outputFrame) {
                    engine.processBlock(inputFrame, outputFrame);
                });
        }

        for (int channel = 0; channel < er301::NUM_OUTPUTS; ++channel)
            outputs[OUT1_OUTPUT + channel].setVoltage(rackOutputs[channel]);
    }

    // Small settings always remain in JSON. Full ER-301 state normally lives
    // in Rack patch storage, but module presets/clipboard JSON need to be
    // self-contained because Rack does not invoke onSave() for those actions.
    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "er301SchemaVersion", json_integer(6));
        json_object_set_new(root, "instanceKey",
                            json_string(instanceKey.c_str()));
        json_object_set_new(root, "scrollRegionMode",
                            json_integer(scrollRegionMode));
        json_object_set_new(root, "panelColor", json_integer(panelColor));
        json_object_set_new(root, "screenColor", json_integer(screenColor));
        json_object_set_new(root, "rackPatchRecall", json_true());

        if (freshStateRequested)
            json_object_set_new(root, "er301FreshState", json_true());

        const bool suppressInline = suppressInlineStateOnce.exchange(
            false, std::memory_order_acq_rel);
        if (!suppressInline && !freshStateRequested) {
            std::vector<uint8_t> state = pendingModulePresetState;
            std::string error;
            if (state.empty() && !captureCurrentState(state, error)) {
                // If the engine is temporarily unavailable, an existing Rack
                // patch-storage snapshot is still useful for Save As/Copy.
                try {
                    const std::string storageDirectory =
                        getPatchStorageDirectory();
                    if (!storageDirectory.empty()) {
                        const std::string storedState = system::join(
                            storageDirectory, kPatchStateFilename);
                        readBinaryFile(storedState, state, error);
                    }
                }
                catch (const Exception&) {
                }
            }

            if (!state.empty()) {
                const std::string encoded = string::toBase64(state);
                json_object_set_new(root, "er301ModulePresetState",
                                    json_stringn(encoded.c_str(),
                                                 encoded.size()));
            }
            else if (!error.empty()) {
                WARN("ER301: module preset state was not embedded: %s",
                     error.c_str());
            }
        }
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* inlineStateJ = json_object_get(
            root, "er301ModulePresetState");
        json_t* freshStateJ = json_object_get(root, "er301FreshState");
        const bool hasInlineState = json_is_string(inlineStateJ);
        const bool requestsFreshState = json_is_true(freshStateJ);
        const bool isModulePresetState = hasInlineState || requestsFreshState;

        // Normal .vcv patch recall restores the private instance identity. A
        // module preset must never clone that identity into another live module.
        json_t* instanceKeyJ = json_object_get(root, "instanceKey");
        if (!isModulePresetState && !instanceKeyClaimed &&
            json_is_string(instanceKeyJ) &&
            json_string_value(instanceKeyJ)[0] != '\0') {
            instanceKey = json_string_value(instanceKeyJ);
        }

        json_t* scrollModeJ = json_object_get(root, "scrollRegionMode");
        if (json_is_integer(scrollModeJ)) {
            int mode = (int)json_integer_value(scrollModeJ);
            scrollRegionMode = std::max((int)SCROLL_ENCODER_ONLY,
                std::min(mode, (int)NUM_SCROLL_REGION_MODES - 1));
        }

        json_t* panelColorJ = json_object_get(root, "panelColor");
        if (json_is_integer(panelColorJ)) {
            const int color = (int)json_integer_value(panelColorJ);
            panelColor = color == PANEL_BLACK ? PANEL_BLACK : PANEL_SILVER;
        }

        json_t* screenColorJ = json_object_get(root, "screenColor");
        if (json_is_integer(screenColorJ)) {
            const int color = (int)json_integer_value(screenColorJ);
            if (color >= 0 && color < NUM_SCREEN_COLORS)
                screenColor = color;
        }

        if (hasInlineState) {
            try {
                std::vector<uint8_t> decoded = string::fromBase64(
                    json_string_value(inlineStateJ));
                if (decoded.empty() || decoded.size() > kMaxInlineStateBytes) {
                    WARN("ER301: ignored empty/oversized module-preset state.");
                }
                else {
                    pendingModulePresetState.swap(decoded);
                    freshStateRequested = false;
                    resetHostLifecycle(false);
                }
            }
            catch (const std::runtime_error& ex) {
                WARN("ER301: invalid module-preset state: %s", ex.what());
            }
        }
        else if (requestsFreshState) {
            pendingModulePresetState.clear();
            freshStateRequested = true;
            // A replayed Initialize/history state should be just as clean as
            // adding a new module, including a fresh private rear/config root.
            resetHostLifecycle(true);
        }

        // Momentary/latch state never persists with a Rack patch or preset.
        shiftLatched = false;
        shiftReleasePending = false;
        params[BUTTON_PARAMS + er301::BTN_SHIFT].setValue(0.f);
        // Force re-applying the toggle positions to the engine after load.
        togglesApplied = false;
    }

};

// Hardware-shaped panel buttons. These forward mouse edges immediately on
// Rack's UI thread, while the module's normal param scan remains available for
// MIDI mapping, automation, and keyboard control.
struct ER301PanelButton : app::SvgSwitch {
    ER301* erModule = NULL;

    ER301PanelButton(const char* released, const char* pressed) {
        momentary = true;
        shadow->opacity = 0.f;
        addFrame(Svg::load(asset::plugin(pluginInstance, released)));
        addFrame(Svg::load(asset::plugin(pluginInstance, pressed)));
    }

    void onButton(const ButtonEvent& e) override {
        app::SvgSwitch::onButton(e);
        if (erModule && e.button == GLFW_MOUSE_BUTTON_LEFT &&
            (e.action == GLFW_PRESS || e.action == GLFW_RELEASE)) {
            erModule->syncButtonsNow();
            if (e.action == GLFW_RELEASE)
                erModule->finishLatchedShiftAction();
        }
    }

    // ParamWidget/OpaqueWidget normally stops wheel propagation even though
    // these momentary buttons do not use the wheel. Let ER301Widget decide
    // whether the gesture should turn the encoder or continue to Rack.
    void onHoverScroll(const HoverScrollEvent& e) override {
        (void)e;
    }
};

struct ER301SoftButton : ER301PanelButton {
    ER301SoftButton() : ER301PanelButton("res/components/soft_0.svg",
                                         "res/components/soft_1.svg") {}
};

struct ER301HardButton : ER301PanelButton {
    ER301HardButton() : ER301PanelButton("res/components/hard_0.svg",
                                         "res/components/hard_1.svg") {}
};

// Mouse-friendly SHIFT latch. A click holds SHIFT until the next completed
// control action; clicking SHIFT again cancels it without sending another key.
struct ER301ShiftButton : ER301HardButton {
    void onButton(const ButtonEvent& e) override {
        if (erModule && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            if (e.action == GLFW_PRESS)
                erModule->toggleShiftLatch();
            e.consume(this);
            return;
        }
        ER301HardButton::onButton(e);
    }
};

struct ER301LinkButton : ER301PanelButton {
    ER301LinkButton() : ER301PanelButton("res/components/link_0.svg",
                                         "res/components/link_1.svg") {}
};

// Visual three-way switch. Selection is handled by a larger transparent
// hit zone below so the printed user/admin/eject and hold/edit/scope legends
// are genuinely clickable, not just the narrow plastic switch body.
struct ER301ThreeSwitch : CKSSThree {
    void onHoverScroll(const HoverScrollEvent& e) override {
        (void)e;
    }
};

// Direct-position selector for the complete printed switch field. Each
// vertical third maps to the corresponding top, middle, or bottom hardware
// position. This preserves the natural "click the word you want" behavior at
// normal Rack zoom levels while leaving the stock CKSSThree artwork intact.
struct ER301ThreeSwitchHitZone : ParamWidget {
    ER301* erModule = NULL;

    void onButton(const ButtonEvent& e) override {
        if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
            if (e.action == GLFW_PRESS) {
                auto* quantity = getParamQuantity();
                if (quantity) {
                    const float third = box.size.y / 3.f;
                    const float value = e.pos.y < third ? 2.f :
                                        e.pos.y < 2.f * third ? 1.f : 0.f;
                    quantity->setValue(value);
                }
                if (erModule)
                    erModule->finishLatchedShiftAction();
            }
            e.consume(this);
            return;
        }
        ParamWidget::onButton(e);
    }

    void onHoverScroll(const HoverScrollEvent& e) override {
        (void)e;
    }
};

// Desktop convenience for the physical front SD-card aperture. This is the
// only custom panel affordance with a tooltip: hovering the slot identifies
// the ER-301 path it represents, and clicking opens the emulated front card.
struct ER301FrontCardTooltip : ui::Tooltip {
    widget::Widget* owner = NULL;

    void step() override {
        ui::Tooltip::step();
        if (!owner || !parent)
            return;
        box.pos = owner->getAbsoluteOffset(Vec(owner->box.size.x, 0.f)).round();
        box = box.nudge(parent->box.zeroPos());
    }
};

struct ER301FrontCardHitZone : OpaqueWidget {
    ER301* erModule = NULL;
    ui::Tooltip* tooltip = NULL;

    void createTooltip() {
        if (!settings::tooltips || tooltip)
            return;
        ER301FrontCardTooltip* t = new ER301FrontCardTooltip;
        t->owner = this;
        t->text = "/front/";
        APP->scene->addChild(t);
        tooltip = t;
    }

    void destroyTooltip() {
        if (!tooltip)
            return;
        if (tooltip->parent)
            tooltip->parent->removeChild(tooltip);
        delete tooltip;
        tooltip = NULL;
    }

    void onEnter(const EnterEvent& e) override {
        (void)e;
        createTooltip();
    }

    void onLeave(const LeaveEvent& e) override {
        (void)e;
        destroyTooltip();
    }

    void onButton(const ButtonEvent& e) override {
        if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS) {
            if (erModule && erModule->runtimePathsReady) {
                system::openDirectory(system::join(
                    erModule->runtimePaths.frontPath, "ER-301"));
            }
            e.consume(this);
            return;
        }
        OpaqueWidget::onButton(e);
    }
};

// Rack ports also inherit OpaqueWidget's wheel propagation stop. The jack
// itself has no wheel behavior, so keep cable interaction unchanged while
// allowing excluded jack regions and modifier gestures to reach Rack.
struct ER301Port : PJ301MPort {
    void onHoverScroll(const HoverScrollEvent& e) override {
        (void)e;
    }
};

// The real panel uses 3 mm recessed indicator lenses. MediumLight supplies the
// correct physical diameter; the custom palette and halo override keep the lens
// restrained rather than producing Rack's broad synthetic bloom.
template <typename TBase>
struct ER301PhysicalLight : MediumLight<TBase> {
    ER301PhysicalLight() {
        this->bgColor = nvgRGB(0x4b, 0x2a, 0x25);
        this->borderColor = nvgRGBA(0x28, 0x28, 0x25, 0xd8);
    }

    void drawHalo(const widget::Widget::DrawArgs& args) override {
        (void)args;
    }
};

using ER301PhysicalRedLight = ER301PhysicalLight<RedLight>;
using ER301PhysicalGreenRedLight = ER301PhysicalLight<GreenRedLight>;

// ===========================================================================
// Display widget: draws the live ER-301 framebuffers with NanoVG.
// UI presentation only. A bridge-owned 55 Hz worker requests and collects
// engine frames; this widget decodes/uploads only the newest published snapshot.
// No display lock or conversion is shared with the Rack audio path.
// ===========================================================================
struct ER301DisplayWidget : TransparentWidget {
    ER301* module = NULL;
    bool isMain = true;

    int cols = 0, rows = 0;
    uint8_t mainRaw[er301::MAIN_BYTES] = {};
    uint8_t subRaw[er301::SUB_BYTES] = {};
    std::vector<uint8_t> rgba;
    int image = -1;
    bool haveFrame = false;

    // Main display uses the ER-301's native 4-bit brightness values. The
    // selected screen tint is applied only after decoding, so every palette
    // keeps the same luminance relationships and anti-aliased detail.
    static constexpr int BRIGHTNESS_STEP = 16; // 15 * 16 = 240
    int screenColor = SCREEN_AMBER;

    ER301DisplayWidget(ER301* m, bool main_) {
        module = m;
        isMain = main_;
        screenColor = loadScreenColorPreference();
        cols = isMain ? er301::MAIN_COLS : er301::SUB_COLS;
        rows = isMain ? er301::MAIN_ROWS : er301::SUB_ROWS;
        rgba.resize(cols * rows * 4, 0);
        // opaque black initial screen
        for (int i = 3; i < (int)rgba.size(); i += 4) rgba[i] = 255;
    }

    void step() override {
        TransparentWidget::step();
        // Only the main display widget pulls frames; the sub widget reuses
        // the module-owned copy? Both widgets share one pull via the main
        // widget's flag would complicate things; instead only the MAIN
        // widget calls the bridge and stores both raw buffers in itself,
        // and the sub widget references the main widget. Simpler: the main
        // widget owns the pull; see ER301Widget which wires 'source'.
    }

    void decodeMain() {
        const uint16_t* src = (const uint16_t*)mainRaw;
        float r, g, b;
        screenColorRGB(screenColor, r, g, b);
        for (int y = 0; y < rows; y++) {
            int yy = rows - y - 1;
            uint8_t* row = &rgba[4 * cols * y];
            for (int x = 0; x < cols; x++) {
                int xx = cols - x - 1;
                uint16_t cell = src[(yy << 7) + (xx >> 1)];
                int shift = (((~xx) & 0b1) << 2);
                int v = ((cell >> shift) & 0xF) * BRIGHTNESS_STEP;
                row[4 * x + 0] = (uint8_t)(v * r + 0.5f);
                row[4 * x + 1] = (uint8_t)(v * g + 0.5f);
                row[4 * x + 2] = (uint8_t)(v * b + 0.5f);
                row[4 * x + 3] = 255;
            }
        }
    }

    void decodeSub() {
        const uint16_t* src = (const uint16_t*)subRaw;
        float r, g, b;
        screenColorRGB(screenColor, r, g, b);
        for (int y = 0; y < rows; y++) {
            int yy = rows - y - 1;
            int shift = yy & 0b111;
            uint8_t* row = &rgba[4 * cols * y];
            for (int x = 0; x < cols; x++) {
                int xx = cols - x - 1;
                uint16_t cell = src[((yy >> 3) << 7) + xx];
                int v = ((cell >> shift) & 0b1) * 255;
                row[4 * x + 0] = (uint8_t)(v * r + 0.5f);
                row[4 * x + 1] = (uint8_t)(v * g + 0.5f);
                row[4 * x + 2] = (uint8_t)(v * b + 0.5f);
                row[4 * x + 3] = 255;
            }
        }
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1) {
            TransparentWidget::drawLayer(args, layer);
            return;
        }
        // Self-lit layer (visible with room lights down, like a real OLED).
        if (image < 0) {
            image = nvgCreateImageRGBA(args.vg, cols, rows, 0, rgba.data());
        }
        else if (haveFrame) {
            nvgUpdateImage(args.vg, image, rgba.data());
            haveFrame = false;
        }
        NVGpaint paint = nvgImagePattern(args.vg, 0.f, 0.f,
                                         box.size.x, box.size.y, 0.f,
                                         image, 1.f);
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0.f, 0.f, box.size.x, box.size.y);
        nvgFillPaint(args.vg, paint);
        nvgFill(args.vg);
    }
};

// ===========================================================================
// Endless encoder. Not bound to a param (an absolute value is meaningless
// for the ER-301's relative encoder); drag/scroll deltas are accumulated
// into the module and forwarded to the engine on the UI thread.
// ===========================================================================
struct ER301EncoderWidget : OpaqueWidget {
    ER301* module = NULL;
    float angle = 0.f;      // visual only
    float dragRemainder = 0.f;

    // Emulator reference: ENCODER_SPEED = 5 counts per UI "step".
    // Fractional trackpad deltas accumulate naturally, while a conventional
    // wheel notch remains five encoder counts like the previous behavior.
    static constexpr float COUNTS_PER_PIXEL = 1.0f;
    static constexpr float COUNTS_PER_WHEEL_UNIT = 5.0f;
    static constexpr float MIN_SCROLL_DELTA = 0.025f;
    static constexpr float VERTICAL_DOMINANCE = 1.20f;
    static constexpr float RADIANS_PER_COUNT = (float)(2.0 * M_PI / 128.0);

    void addCounts(float c) {
        dragRemainder += c;
        int whole = (int)dragRemainder;
        if (whole != 0) {
            dragRemainder -= whole;
            angle += whole * RADIANS_PER_COUNT;
            if (module)
                module->turnEncoderNow(whole);
        }
    }

    void onButton(const ButtonEvent& e) override {
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            e.consume(this);
        }
        OpaqueWidget::onButton(e);
    }

    void onDragStart(const DragStartEvent& e) override {
        APP->window->cursorLock();
    }

    void onDragEnd(const DragEndEvent& e) override {
        APP->window->cursorUnlock();
    }

    void onDragMove(const DragMoveEvent& e) override {
        // Vertical drag, like a Rack knob: up = clockwise/increment.
        addCounts(-e.mouseDelta.y * COUNTS_PER_PIXEL);
    }

    static bool rackModifierHeld() {
        return APP && APP->window &&
               ((APP->window->getMods() & RACK_MOD_MASK) != 0);
    }

    bool addScrollDelta(Vec delta) {
        const float ax = std::fabs(delta.x);
        const float ay = std::fabs(delta.y);
        if (ay < MIN_SCROLL_DELTA || ay < ax * VERTICAL_DOMINANCE)
            return false;

        // Clamp one event so high-resolution trackpads cannot jump through a
        // long menu in a single callback. Small fractional deltas are kept
        // and accumulated by addCounts().
        const float units = std::max(-1.f, std::min(delta.y, 1.f));
        addCounts(units * COUNTS_PER_WHEEL_UNIT);
        return true;
    }

    void onHoverScroll(const HoverScrollEvent& e) override {
        // Modified scroll gestures belong to Rack (zoom/pan). Horizontal or
        // near-horizontal trackpad gestures also pass through untouched.
        if (rackModifierHeld() || !addScrollDelta(e.scrollDelta))
            return;
        e.consume(this);
    }

    void draw(const DrawArgs& args) override {
        float r = std::min(box.size.x, box.size.y) * 0.5f;
        float cx = box.size.x * 0.5f, cy = box.size.y * 0.5f;
        // body
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, cx, cy, r);
        nvgFillColor(args.vg, nvgRGB(0x2b, 0x2b, 0x2b));
        nvgFill(args.vg);
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, cx, cy, r);
        nvgStrokeWidth(args.vg, 1.2f);
        nvgStrokeColor(args.vg, nvgRGB(0x11, 0x11, 0x11));
        nvgStroke(args.vg);
        // knurl hint
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, cx, cy, r * 0.82f);
        nvgStrokeWidth(args.vg, 0.8f);
        nvgStrokeColor(args.vg, nvgRGB(0x3a, 0x3a, 0x3a));
        nvgStroke(args.vg);
        // indicator
        float ix = cx + std::sin(angle) * r * 0.75f;
        float iy = cy - std::cos(angle) * r * 0.75f;
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, ix, iy, r * 0.09f);
        nvgFillColor(args.vg, nvgRGB(0xd8, 0xd8, 0xd8));
        nvgFill(args.vg);
    }
};

// ===========================================================================
// Panel silkscreen. Rack's SVG loader does not render <text>, so all
// faceplate labels are drawn here with NanoVG on the panel layer.
// ===========================================================================
struct ER301LabelsWidget : TransparentWidget {
    struct Label {
        float x, y;      // mm
        float size;      // px
        const char* text;
        int align;       // horizontal NanoVG alignment
        float spacing;   // px letter spacing
    };
    struct BoxLabel {
        float x, y;      // mm, center
        float w, h;      // mm
        float size;      // px
        const char* text;
        float spacing;   // px letter spacing
    };
    struct FooterLabel {
        float x, y;      // mm
        float size;      // px
        const char* text;
        int align;
        float spacing;
        float scaleX;
        float embolden;
    };

    std::vector<Label> labels;
    std::vector<BoxLabel> boxedLabels;
    std::vector<FooterLabel> footerLabels;
    std::shared_ptr<Font> font;
    std::shared_ptr<Font> footerFont;
    bool blackPanel = false;

    // Font scaling is applied in draw(): ordinary legends receive a restrained
    // enlargement, while boxed legends receive a smaller increase so they
    // continue to clear their outlines.

    void add(float xmm, float ymm, float size, const char* text,
             int align = NVG_ALIGN_CENTER, float spacing = 0.f) {
        labels.push_back({xmm, ymm, size, text, align, spacing});
    }

    void addLeft(float xmm, float ymm, float size, const char* text,
                 float spacing = 0.f) {
        add(xmm, ymm, size, text, NVG_ALIGN_LEFT, spacing);
    }

    void addBox(float xmm, float ymm, float wmm, float hmm,
                float size, const char* text, float spacing = 0.f) {
        boxedLabels.push_back({xmm, ymm, wmm, hmm, size, text, spacing});
    }

    void addFooter(float xmm, float ymm, float size, const char* text,
                   int align = NVG_ALIGN_LEFT, float spacing = 0.f,
                   float scaleX = 1.f, float embolden = 0.f) {
        footerLabels.push_back(
            {xmm, ymm, size, text, align, spacing, scaleX, embolden});
    }

    void draw(const DrawArgs& args) override {
        if (!font)
            font = APP->window->loadFont(
                asset::system("res/fonts/ShareTechMono-Regular.ttf"));
        if (!footerFont)
            footerFont = APP->window->loadFont(
                asset::system("res/fonts/DejaVuSans.ttf"));
        if (!font)
            return;

        const NVGcolor ink = blackPanel
            ? nvgRGB(0xd8, 0xd8, 0xd5)
            : nvgRGB(0x1c, 0x1d, 0x1b);
        const NVGcolor panelCutout = blackPanel
            ? nvgRGB(0x14, 0x14, 0x14)
            : nvgRGB(0xc3, 0xc4, 0xc0);
        const NVGcolor boxOutline = blackPanel
            ? nvgRGB(0x9a, 0x9a, 0x96)
            : nvgRGB(0x3a, 0x3b, 0x38);
        const float labelFontScale = 1.10f;
        const float boxFontScale = 1.06f;
        nvgFontFaceId(args.vg, font->handle);

        // Small outlined legends used throughout the Original Flavor panel.
        for (const BoxLabel& b : boxedLabels) {
            Vec c = mm2px(Vec(b.x, b.y));
            Vec sz = mm2px(Vec(b.w, b.h));
            nvgBeginPath(args.vg);
            nvgRoundedRect(args.vg, c.x - sz.x * 0.5f, c.y - sz.y * 0.5f,
                           sz.x, sz.y, mm2px(0.43f));
            nvgStrokeWidth(args.vg, 0.82f);
            nvgStrokeColor(args.vg, boxOutline);
            nvgStroke(args.vg);

            nvgFontSize(args.vg, b.size * boxFontScale);
            nvgTextLetterSpacing(args.vg, b.spacing);
            nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(args.vg, ink);
            nvgText(args.vg, c.x, c.y, b.text, NULL);
        }

        for (const Label& l : labels) {
            nvgFontSize(args.vg, l.size * labelFontScale);
            nvgTextLetterSpacing(args.vg, l.spacing);
            nvgTextAlign(args.vg, l.align | NVG_ALIGN_MIDDLE);
            nvgFillColor(args.vg, ink);
            Vec p = mm2px(Vec(l.x, l.y));
            nvgText(args.vg, p.x, p.y, l.text, NULL);
        }

        // The hardware footer is a compact, heavy, proportional face. Rack's
        // bundled DejaVu Sans is safely available on every installation, so a
        // slight horizontal compression and restrained overdraw reproduce the
        // original engraving without shipping an extra font file.
        nvgFontFaceId(args.vg, footerFont ? footerFont->handle : font->handle);
        for (const FooterLabel& l : footerLabels) {
            nvgFontSize(args.vg, l.size);
            nvgTextLetterSpacing(args.vg, l.spacing);
            nvgTextAlign(args.vg, l.align | NVG_ALIGN_MIDDLE);
            nvgFillColor(args.vg, ink);
            Vec p = mm2px(Vec(l.x, l.y));
            nvgSave(args.vg);
            nvgTranslate(args.vg, p.x, p.y);
            nvgScale(args.vg, l.scaleX, 1.f);
            nvgText(args.vg, 0.f, 0.f, l.text, NULL);
            if (l.embolden > 0.f) {
                nvgText(args.vg, l.embolden, 0.f, l.text, NULL);
                nvgText(args.vg, -l.embolden, 0.f, l.text, NULL);
            }
            nvgRestore(args.vg);
        }
        nvgTextLetterSpacing(args.vg, 0.f);
        nvgFontFaceId(args.vg, font->handle);

        auto strokeLine = [&](float x1, float y1, float x2, float y2,
                              float width = 0.78f) {
            Vec a = mm2px(Vec(x1, y1));
            Vec b = mm2px(Vec(x2, y2));
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, a.x, a.y);
            nvgLineTo(args.vg, b.x, b.y);
            nvgStrokeWidth(args.vg, width);
            nvgStrokeColor(args.vg, ink);
            nvgStroke(args.vg);
        };
        auto triangle = [&](float x, float y, float halfW, float halfH,
                            int direction) {
            Vec a, b, c;
            if (direction == 0) { // up
                a = mm2px(Vec(x, y - halfH));
                b = mm2px(Vec(x - halfW, y + halfH));
                c = mm2px(Vec(x + halfW, y + halfH));
            }
            else if (direction == 1) { // down
                a = mm2px(Vec(x, y + halfH));
                b = mm2px(Vec(x - halfW, y - halfH));
                c = mm2px(Vec(x + halfW, y - halfH));
            }
            else if (direction == 2) { // left
                a = mm2px(Vec(x - halfW, y));
                b = mm2px(Vec(x + halfW, y - halfH));
                c = mm2px(Vec(x + halfW, y + halfH));
            }
            else { // right
                a = mm2px(Vec(x + halfW, y));
                b = mm2px(Vec(x - halfW, y - halfH));
                c = mm2px(Vec(x - halfW, y + halfH));
            }
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, a.x, a.y);
            nvgLineTo(args.vg, b.x, b.y);
            nvgLineTo(args.vg, c.x, c.y);
            nvgClosePath(args.vg);
            nvgFillColor(args.vg, ink);
            nvgFill(args.vg);
        };
        auto dot = [&](float x, float y, float radiusMm = 0.28f) {
            Vec c = mm2px(Vec(x, y));
            nvgBeginPath(args.vg);
            nvgCircle(args.vg, c.x, c.y, mm2px(radiusMm));
            nvgFillColor(args.vg, ink);
            nvgFill(args.vg);
        };

        // Fine/coarse direction marks and underlines.
        strokeLine(2.45f, 85.05f, 6.40f, 85.05f, 0.68f);
        strokeLine(4.43f, 85.65f, 4.43f, 87.22f, 0.72f);
        triangle(4.43f, 85.60f, 0.36f, 0.38f, 0);
        triangle(4.43f, 87.27f, 0.36f, 0.38f, 1);
        strokeLine(5.65f, 89.22f, 11.15f, 89.22f, 0.68f);
        strokeLine(7.15f, 90.16f, 9.75f, 90.16f, 0.72f);
        triangle(7.10f, 90.16f, 0.40f, 0.34f, 2);
        triangle(9.80f, 90.16f, 0.40f, 0.34f, 3);

        // Home symbol above ZERO.
        Vec h0 = mm2px(Vec(36.35f, 87.45f));
        Vec h1 = mm2px(Vec(38.00f, 85.95f));
        Vec h2 = mm2px(Vec(39.65f, 87.45f));
        Vec h3 = mm2px(Vec(39.15f, 87.45f));
        Vec h4 = mm2px(Vec(39.15f, 89.05f));
        Vec h5 = mm2px(Vec(36.85f, 89.05f));
        Vec h6 = mm2px(Vec(36.85f, 87.45f));
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, h0.x, h0.y);
        nvgLineTo(args.vg, h1.x, h1.y);
        nvgLineTo(args.vg, h2.x, h2.y);
        nvgLineTo(args.vg, h3.x, h3.y);
        nvgLineTo(args.vg, h4.x, h4.y);
        nvgLineTo(args.vg, h5.x, h5.y);
        nvgLineTo(args.vg, h6.x, h6.y);
        nvgClosePath(args.vg);
        nvgFillColor(args.vg, ink);
        nvgFill(args.vg);
        Vec door = mm2px(Vec(38.0f, 88.35f));
        nvgBeginPath(args.vg);
        nvgRect(args.vg, door.x - mm2px(0.30f), door.y - mm2px(0.20f),
                mm2px(0.60f), mm2px(0.90f));
        nvgFillColor(args.vg, panelCutout);
        nvgFill(args.vg);

        // UP symbol and Storage card mark. The hardware UP arrow has a
        // short stalk; it is not merely a floating triangle.
        triangle(64.20f, 106.10f, 0.62f, 0.72f, 0);
        strokeLine(64.20f, 106.45f, 64.20f, 107.28f, 0.92f);
        Vec card = mm2px(Vec(17.30f, 103.85f));
        Vec cardSize = mm2px(Vec(2.65f, 2.05f));
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, card.x, card.y, cardSize.x, cardSize.y,
                       mm2px(0.18f));
        nvgFillColor(args.vg, ink);
        nvgFill(args.vg);
        Vec cardTri = mm2px(Vec(18.75f, 104.88f));
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, cardTri.x + mm2px(0.45f), cardTri.y);
        nvgLineTo(args.vg, cardTri.x - mm2px(0.45f),
                  cardTri.y - mm2px(0.42f));
        nvgLineTo(args.vg, cardTri.x - mm2px(0.45f),
                  cardTri.y + mm2px(0.42f));
        nvgClosePath(args.vg);
        nvgFillColor(args.vg, panelCutout);
        nvgFill(args.vg);

        // Storage and Mode three-position legends.
        triangle(10.90f, 109.25f, 0.34f, 0.40f, 0);
        dot(10.90f, 113.08f, 0.25f);
        triangle(10.90f, 116.90f, 0.34f, 0.40f, 1);
        triangle(39.70f, 109.25f, 0.34f, 0.40f, 0);
        dot(39.70f, 113.08f, 0.25f);
        triangle(39.70f, 116.90f, 0.34f, 0.40f, 1);
    }
};

// ===========================================================================
// Host text-entry popup
// ===========================================================================

static bool hostTextCharacterAllowed(uint32_t cp, bool extended) {
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
        (cp >= '0' && cp <= '9') || cp == ' ' || cp == '+' || cp == '-' ||
        cp == '_' || cp == '=' || cp == '~' || cp == '.')
        return true;
    if (!extended)
        return false;
    switch (cp) {
    case '!': case '@': case '#': case '$': case '%': case '^':
    case '&': case '*': case '(': case ')': case '[': case ']':
    case '/': case '\\': case '|': case ':': case '<': case '>':
    case '?': case ',': case ';': case '"': case '\'':
    case 0x00b1: // ±, present on the ER-301 extended keyboard.
        return true;
    default:
        return false;
    }
}

static std::string filterHostText(const std::string& input, bool extended) {
    std::u32string decoded = string::UTF8toUTF32(input);
    std::u32string filtered;
    filtered.reserve(decoded.size());
    for (uint32_t cp : decoded) {
        if (hostTextCharacterAllowed(cp, extended))
            filtered.push_back((char32_t)cp);
    }
    return string::UTF32toUTF8(filtered);
}

struct ER301HostTextMenu;

struct ER301HostTextField : ui::TextField {
    ER301HostTextMenu* owner = nullptr;
    bool extended = false;

    void sanitize() {
        const std::string filtered = filterHostText(text, extended);
        if (filtered == text)
            return;
        setText(filtered);
        cursor = (int)text.size();
        selection = cursor;
    }

    void onSelectText(const SelectTextEvent& e) override {
        if (!hostTextCharacterAllowed(e.codepoint, extended)) {
            e.consume(this);
            return;
        }
        ui::TextField::onSelectText(e);
    }

    void onSelectKey(const SelectKeyEvent& e) override;
};

struct ER301HostTextEntry : ui::MenuEntry {
    ER301HostTextField* field = nullptr;

    ER301HostTextEntry() {
        box.size = Vec(360.f, 34.f);
        field = new ER301HostTextField;
        field->box.pos = Vec(6.f, 4.f);
        field->box.size = Vec(348.f, 26.f);
        addChild(field);
    }
};

struct ER301HostTextMenu : ui::Menu {
    ER301* erModule = nullptr;
    ER301HostTextField* field = nullptr;
    bool resolved = false;

    ~ER301HostTextMenu() override {
        // Clicking outside a Rack menu destroys it without invoking a menu
        // item. Treat that exactly like Cancel so the engine handshake cannot
        // remain stuck in OPEN state.
        if (!resolved && erModule)
            erModule->engine.completeHostTextDialog(std::string(), true);
    }

    void finish(bool cancelled) {
        if (resolved)
            return;
        resolved = true;
        if (erModule) {
            const std::string value = field ? filterHostText(field->getText(),
                                                             field->extended)
                                            : std::string();
            erModule->engine.completeHostTextDialog(value, cancelled);
        }
        if (ui::MenuOverlay* overlay = getAncestorOfType<ui::MenuOverlay>())
            overlay->requestDelete();
    }
};

void ER301HostTextField::onSelectKey(const SelectKeyEvent& e) {
    if ((e.action == GLFW_PRESS || e.action == GLFW_REPEAT) && owner) {
        if (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER) {
            owner->finish(false);
            e.consume(this);
            return;
        }
        if (e.key == GLFW_KEY_ESCAPE) {
            owner->finish(true);
            e.consume(this);
            return;
        }
    }
    ui::TextField::onSelectKey(e);
    // TextField handles paste internally on SelectKey. Sanitize after it so
    // pasted text obeys the same character set as the ER-301 keyboard.
    sanitize();
}

// ===========================================================================
// ModuleWidget
// ===========================================================================
struct ER301Widget : ModuleWidget {
    ER301DisplayWidget* mainDisplay = NULL;
    ER301DisplayWidget* subDisplay = NULL;
    ER301EncoderWidget* encoder = NULL;
    app::SvgPanel* faceplate = NULL;
    ER301LabelsWidget* labelsWidget = NULL;
    int panelColor = PANEL_SILVER;
    int screenColor = SCREEN_AMBER;
    uint8_t nextMain[er301::MAIN_BYTES] = {};
    uint8_t nextSub[er301::SUB_BYTES] = {};

    void applyPanelColor(int color, bool persist) {
        panelColor = color == PANEL_BLACK ? PANEL_BLACK : PANEL_SILVER;
        if (ER301* m = getModule<ER301>())
            m->panelColor = panelColor;
        if (persist)
            savePanelColorPreference(panelColor);

        if (faceplate) {
            const char* resource = panelColor == PANEL_BLACK
                ? "res/ER301Black.svg"
                : "res/ER301.svg";
            faceplate->setBackground(
                window::Svg::load(asset::plugin(pluginInstance, resource)));
        }
        if (labelsWidget)
            labelsWidget->blackPanel = panelColor == PANEL_BLACK;
    }

    void applyScreenColor(int color, bool persist) {
        if (color < 0 || color >= NUM_SCREEN_COLORS)
            color = SCREEN_AMBER;
        screenColor = color;
        if (ER301* m = getModule<ER301>())
            m->screenColor = screenColor;
        if (persist)
            saveScreenColorPreference(screenColor);

        // Re-tint the already-published frame immediately. This is important
        // for static ER-301 screens, whose raw framebuffer may not otherwise
        // change after the user picks a new color.
        if (mainDisplay) {
            mainDisplay->screenColor = screenColor;
            mainDisplay->decodeMain();
            mainDisplay->haveFrame = true;
        }
        if (subDisplay) {
            subDisplay->screenColor = screenColor;
            subDisplay->decodeSub();
            subDisplay->haveFrame = true;
        }
    }

    static bool insideMmRect(Vec p, float x, float y, float w, float h) {
        const Vec topLeft = mm2px(Vec(x, y));
        const Vec size = mm2px(Vec(w, h));
        return p.x >= topLeft.x && p.y >= topLeft.y &&
               p.x < topLeft.x + size.x && p.y < topLeft.y + size.y;
    }

    static bool nearJack(Vec p, er301layout::XY jack, float radiusMm = 6.2f) {
        const Vec center = mm2px(Vec(jack.x, jack.y));
        const Vec d = p.minus(center);
        const float radius = mm2px(radiusMm);
        return d.x * d.x + d.y * d.y < radius * radius;
    }

    bool inJackExclusion(Vec p) const {
        using namespace er301layout;
        for (int i = 0; i < 4; i++) {
            if (nearJack(p, G[i]) || nearJack(p, IN[i]) || nearJack(p, OUT[i]))
                return true;
        }
        const XY* columns[4] = {A, B, C, D};
        for (int col = 0; col < 4; col++) {
            for (int row = 0; row < 3; row++) {
                if (nearJack(p, columns[col][row]))
                    return true;
            }
        }
        return false;
    }

    bool routesScrollAt(Vec p) {
        ER301* erModule = getModule<ER301>();
        if (!erModule)
            return false;

        switch (erModule->scrollRegionMode) {
        case ER301::SCROLL_ENCODER_ONLY:
            return false; // The encoder child handles its own wheel events.
        case ER301::SCROLL_CONTROLS:
            // Main/sub displays, encoder, buttons, toggles, and the useful
            // blank areas of the left control section. The jack bank begins
            // just to the right of this rectangle.
            return insideMmRect(p, 4.0f, 9.0f, 84.5f, 114.0f);
        case ER301::SCROLL_MOST_PANEL:
            return insideMmRect(p, 3.0f, 7.0f,
                                er301layout::PANEL_W - 6.0f, 117.0f) &&
                   !inJackExclusion(p);
        default:
            return false;
        }
    }

    void onHoverScroll(const HoverScrollEvent& e) override {
        // Recurse without OpaqueWidget's unconditional propagation stop.
        // The physical encoder gets first chance and consumes its own event.
        widget::Widget::onHoverScroll(e);
        if (e.isConsumed() || !e.isPropagating())
            return;

        if (!encoder || ER301EncoderWidget::rackModifierHeld() ||
            !routesScrollAt(e.pos))
            return;

        if (encoder->addScrollDelta(e.scrollDelta))
            e.consume(this);
    }

    void serviceHostTextDialog() {
        ER301* erModule = getModule<ER301>();
        if (!erModule || !erModule->engine.isReady())
            return;

        std::string message;
        std::string initialText;
        bool extended = false;
        if (!erModule->engine.takeHostTextDialogRequest(message, initialText,
                                                        extended))
            return;

        ER301HostTextMenu* menu = createMenu<ER301HostTextMenu>();
        menu->erModule = erModule;
        menu->box.pos = APP->scene->mousePos.plus(Vec(12.f, 12.f));

        ui::MenuLabel* title = createMenuLabel(
            message.empty() ? "ER-301 text entry" : message);
        menu->addChild(title);

        ER301HostTextEntry* entry = new ER301HostTextEntry;
        entry->field->owner = menu;
        entry->field->extended = extended;
        entry->field->setText(filterHostText(initialText, extended));
        entry->field->selectAll();
        menu->field = entry->field;
        menu->addChild(entry);

        menu->addChild(createMenuItem(
            "OK", "Enter", [menu]() { menu->finish(false); }, false, true));
        menu->addChild(createMenuItem(
            "Cancel", "Esc", [menu]() { menu->finish(true); }, false, true));

        // Give keyboard focus immediately so the user can just type.
        if (APP && APP->event)
            APP->event->setSelectedWidget(entry->field);
    }

    void appendContextMenu(ui::Menu* menu) override {
        ER301* erModule = getModule<ER301>();

        // Rack constructs its standard Info submenu before calling a module's
        // appendContextMenu(). Replace only that submenu wrapper so we can keep
        // Rack's own metadata entries verbatim and insert the upstream ER-301
        // repository immediately after the VCV port's Source code entry.
        ui::MenuItem* stockInfo = NULL;
        const std::string infoText = string::translate("ModuleWidget.info");
        for (widget::Widget* child : menu->children) {
            ui::MenuItem* item = dynamic_cast<ui::MenuItem*>(child);
            if (item && item->text == infoText) {
                stockInfo = item;
                break;
            }
        }
        if (stockInfo && model) {
            plugin::Model* infoModel = model;
            WeakPtr<ER301Widget> weakThis = this;
            ui::MenuItem* replacementInfo = createSubmenuItem(
                infoText, "", [=](ui::Menu* infoMenu) {
                    infoModel->appendContextMenu(infoMenu);

                    ui::MenuItem* sourceItem = NULL;
                    const std::string sourceText = string::translate("Model.source");
                    for (widget::Widget* child : infoMenu->children) {
                        ui::MenuItem* item = dynamic_cast<ui::MenuItem*>(child);
                        if (item && item->text == sourceText) {
                            sourceItem = item;
                            break;
                        }
                    }

                    ui::MenuItem* originalRepository = createMenuItem(
                        "Original Repository", "", []() {
                            system::openBrowser("https://github.com/odevices/er-301");
                        });
                    if (sourceItem)
                        infoMenu->addChildAbove(originalRepository, sourceItem);
                    else
                        infoMenu->addChild(originalRepository);

                    if (!weakThis || !weakThis->module)
                        return;
                    infoMenu->addChild(new ui::MenuSeparator);
                    infoMenu->addChild(createMenuLabel(
                        string::translate("ModuleWidget.moduleId")));
                    infoMenu->addChild(createMenuLabel(string::f(
                        "%lld", (long long) weakThis->module->getId())));
                });
            menu->addChildBelow(replacementInfo, stockInfo);
            menu->removeChild(stockInfo);
            delete stockInfo;
        }

        menu->addChild(new ui::MenuSeparator);
        if (erModule && erModule->engine.slotNumber() > 0) {
            menu->addChild(createMenuLabel(string::f(
                "Isolated engine: slot %u", erModule->engine.slotNumber())));
        }
        else {
            menu->addChild(createMenuLabel("Isolated engine: initializing"));
        }
        if (erModule && erModule->nativeSampleRate > 0) {
            const int rackRate = erModule->rackSampleRate.load(
                std::memory_order_acquire);
            menu->addChild(createMenuLabel(string::f(
                "Audio: Rack %d Hz / ER-301 %d Hz%s",
                rackRate, erModule->nativeSampleRate,
                rackRate == erModule->nativeSampleRate
                    ? " (native)" : " (converted)")));
        }
        else {
            menu->addChild(createMenuLabel("Audio: initializing"));
        }
        menu->addChild(createMenuItem(
            "Open front", "", [=]() {
                if (erModule && erModule->runtimePathsReady) {
                    system::openDirectory(system::join(
                        erModule->runtimePaths.frontPath, "ER-301"));
                }
            }, erModule == NULL || !erModule->runtimePathsReady));
        menu->addChild(createIndexSubmenuItem(
            "Panel color",
            {"Silver", "Black"},
            [=]() {
                return (size_t)panelColor;
            },
            [=](size_t color) {
                if (color < NUM_PANEL_COLORS)
                    applyPanelColor((int)color, true);
            }));
        menu->addChild(createIndexSubmenuItem(
            "Screen color",
            {"Amber", "Tan", "Cyan", "Blue", "Green", "Mint",
             "Red", "Magenta", "Rose", "Crystal"},
            [=]() {
                return (size_t)screenColor;
            },
            [=](size_t color) {
                if (color < NUM_SCREEN_COLORS)
                    applyScreenColor((int)color, true);
            }));
        menu->addChild(createIndexSubmenuItem(
            "Mouse wheel / trackpad",
            {"Encoder only", "Displays and controls",
             "Most panel (except jacks)"},
            [=]() {
                return erModule ? (size_t)erModule->scrollRegionMode :
                                  (size_t)ER301::SCROLL_CONTROLS;
            },
            [=](size_t mode) {
                if (erModule && mode < ER301::NUM_SCROLL_REGION_MODES)
                    erModule->scrollRegionMode = (int)mode;
            },
            erModule == NULL));

    }

    ER301Widget(ER301* module) {
        setModule(module);

        // Fresh modules inherit the sticky defaults. Restored modules may
        // receive their saved colors in dataFromJson() shortly after widget
        // construction; step() below then applies that per-instance state.
        panelColor = module && module->panelColor >= 0
            ? module->panelColor : loadPanelColorPreference();
        screenColor = module && module->screenColor >= 0
            ? module->screenColor : loadScreenColorPreference();
        if (module) {
            module->panelColor = panelColor;
            module->screenColor = screenColor;
        }
        const char* panelResource = panelColor == PANEL_BLACK
            ? "res/ER301Black.svg"
            : "res/ER301.svg";
        faceplate = createPanel(asset::plugin(pluginInstance, panelResource));
        setPanel(faceplate);

        for (const er301layout::XY& p : er301layout::SCREW) {
            addChild(createWidgetCentered<ScrewSilver>(
                mm2px(Vec(p.x, p.y))));
        }

        using namespace er301layout;

        // Displays
        mainDisplay = new ER301DisplayWidget(module, true);
        mainDisplay->screenColor = screenColor;
        mainDisplay->box.pos = mm2px(Vec(MAIN_DISP[0], MAIN_DISP[1]));
        mainDisplay->box.size = mm2px(Vec(MAIN_DISP[2], MAIN_DISP[3]));
        addChild(mainDisplay);

        subDisplay = new ER301DisplayWidget(module, false);
        subDisplay->screenColor = screenColor;
        subDisplay->box.pos = mm2px(Vec(SUB_DISP[0], SUB_DISP[1]));
        subDisplay->box.size = mm2px(Vec(SUB_DISP[2], SUB_DISP[3]));
        addChild(subDisplay);

        // Encoder
        encoder = new ER301EncoderWidget();
        encoder->module = module;
        encoder->box.size = mm2px(Vec(KNOB_DIAMETER, KNOB_DIAMETER));
        encoder->box.pos = mm2px(Vec(KNOB.x - KNOB_DIAMETER * 0.5f,
                                     KNOB.y - KNOB_DIAMETER * 0.5f));
        addChild(encoder);

        // Buttons. M1-M6, S1-S3, and the four channel selectors are gray
        // on the Original Flavor panel. The lower navigation keys are blue.
        auto softBtn = [&](XY p, int which) {
            ER301SoftButton* w = createParamCentered<ER301SoftButton>(
                mm2px(Vec(p.x, p.y)), module, ER301::BUTTON_PARAMS + which);
            w->erModule = module;
            addParam(w);
        };
        auto hardBtn = [&](XY p, int which) {
            ER301HardButton* w = createParamCentered<ER301HardButton>(
                mm2px(Vec(p.x, p.y)), module, ER301::BUTTON_PARAMS + which);
            w->erModule = module;
            addParam(w);
        };
        for (int i = 0; i < 6; i++) softBtn(M[i], er301::BTN_M1 + i);
        hardBtn(DIAL[0], er301::BTN_DIAL1);
        hardBtn(DIAL[1], er301::BTN_CANCEL);
        hardBtn(DIAL[2], er301::BTN_HOME);
        for (int i = 0; i < 3; i++) softBtn(S[i], er301::BTN_S1 + i);
        hardBtn(HB[0], er301::BTN_ENTER);
        hardBtn(HB[1], er301::BTN_UP);
        ER301ShiftButton* shift = createParamCentered<ER301ShiftButton>(
            mm2px(Vec(HB[2].x, HB[2].y)), module,
            ER301::BUTTON_PARAMS + er301::BTN_SHIFT);
        shift->erModule = module;
        addParam(shift);
        for (int i = 0; i < 4; i++) softBtn(SELECT[i], er301::BTN_SELECT1 + i);

        // Mouse-friendly stereo-link chords. The SVGs are intentionally
        // invisible because the hardware has no extra link buttons; the
        // clickable regions sit over the printed "linked" labels.
        for (int i = 0; i < 3; i++) {
            ER301LinkButton* w = createParamCentered<ER301LinkButton>(
                mm2px(Vec(LINK[i].x, LINK[i].y)), module,
                ER301::LINK_PARAMS + i);
            w->erModule = module;
            addParam(w);
        }

        // 3-position switches. The visible hardware controls remain narrow,
        // but transparent selectors cover both the switch and its three
        // printed choices. The selector height is registered to the label
        // baselines so its thirds center on user/admin/eject and
        // hold/edit/scope respectively.
        addParam(createParamCentered<ER301ThreeSwitch>(
            mm2px(Vec(TOG_STORAGE.x, TOG_STORAGE.y)), module,
            ER301::STORAGE_PARAM));
        addParam(createParamCentered<ER301ThreeSwitch>(
            mm2px(Vec(TOG_MODE.x, TOG_MODE.y)), module,
            ER301::MODE_PARAM));

        auto addThreeSwitchHitZone = [&](float centerX, float widthMm,
                                         int paramId) {
            ER301ThreeSwitchHitZone* zone =
                createParam<ER301ThreeSwitchHitZone>(Vec(0.f, 0.f), module,
                                                     paramId);
            zone->box.size = mm2px(Vec(widthMm, 11.70f));
            zone->box.pos = mm2px(Vec(centerX, 113.08f)).minus(
                zone->box.size.mult(0.5f));
            zone->erModule = module;
            addParam(zone);
        };
        addThreeSwitchHitZone(12.25f, 15.50f, ER301::STORAGE_PARAM);
        addThreeSwitchHitZone(40.65f, 14.30f, ER301::MODE_PARAM);

        // The tall physical SD-card aperture itself opens /front/. These
        // coordinates match the aperture rectangle in both panel SVGs exactly;
        // the small printed symbol above it remains non-interactive.
        ER301FrontCardHitZone* frontCard = new ER301FrontCardHitZone();
        frontCard->erModule = module;
        frontCard->box.pos = mm2px(Vec(20.50f, 106.75f));
        frontCard->box.size = mm2px(Vec(3.00f, 12.35f));
        addChild(frontCard);

        // Inputs / outputs
        auto in = [&](XY p, int jack) {
            addInput(createInputCentered<ER301Port>(mm2px(Vec(p.x, p.y)), module,
                                                     ER301::JACK_INPUTS + jack));
        };
        for (int i = 0; i < 4; i++) in(G[i], er301::JACK_G1 + i);
        for (int i = 0; i < 4; i++) in(IN[i], er301::JACK_IN1 + i);
        for (int i = 0; i < 3; i++) in(A[i], er301::JACK_A1 + i);
        for (int i = 0; i < 3; i++) in(B[i], er301::JACK_B1 + i);
        for (int i = 0; i < 3; i++) in(C[i], er301::JACK_C1 + i);
        for (int i = 0; i < 3; i++) in(D[i], er301::JACK_D1 + i);
        for (int i = 0; i < 4; i++) {
            addOutput(createOutputCentered<ER301Port>(
                mm2px(Vec(OUT[i].x, OUT[i].y)), module, ER301::OUT1_OUTPUT + i));
        }

        // Panel LEDs
        auto led = [&](XY p, int which) {
            addChild(createLightCentered<ER301PhysicalRedLight>(
                mm2px(Vec(p.x, p.y)), module, ER301::PANEL_LIGHTS + which));
        };
        led(LED_FINE, er301::PLED_FINE);
        led(LED_COARSE, er301::PLED_COARSE);
        led(LED_IO, er301::PLED_IO);
        led(LED_SAFE, er301::PLED_SAFE);
        for (int i = 0; i < 4; i++) {
            addChild(createLightCentered<ER301PhysicalRedLight>(
                mm2px(Vec(LED_OUT[i].x, LED_OUT[i].y)), module,
                ER301::PANEL_LIGHTS + er301::PLED_OUT1 + i));
        }
        for (int i = 0; i < 3; i++) {
            led(LED_LINK[i], er301::PLED_LINK12 + i);
        }
        // ABCD input level LEDs: to the left of each jack in the lower bank.
        // PWM channel order is A1,B1,C1,D1,A2,B2,... (od/UIThread.cpp).
        const XY* cols[4] = {A, B, C, D};
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 4; col++) {
                int pwm = row * 4 + col;
                const XY p = ABCD_LED[col][row];
                addChild(createLightCentered<ER301PhysicalGreenRedLight>(
                    mm2px(Vec(p.x, p.y)), module,
                    ER301::ABCD_LIGHTS + 2 * pwm));
            }
        }

        // Original Flavor silkscreen labels and footer engraving, positioned
        // against the physical ER-301 panel reference.
        ER301LabelsWidget* text = new ER301LabelsWidget();
        labelsWidget = text;
        text->blackPanel = panelColor == PANEL_BLACK;
        text->box.pos = Vec(0, 0);
        text->box.size = box.size;

        // Original Flavor header: two compact engravings positioned to match
        // the physical ER-301 faceplate.
        text->add(61.15f, 4.72f, 9.50f, "ER-301",
                  NVG_ALIGN_CENTER, 0.10f);
        // The hardware's SOUND COMPUTER engraving is visibly smaller than
        // ER-301 and sits close beside it; preserve both the size hierarchy
        // and the compact Original Flavor title spacing.
        text->add(79.05f, 4.72f, 8.00f, "SOUND COMPUTER",
                  NVG_ALIGN_CENTER, 0.14f);
        text->add(G[0].x, 10.45f, 6.65f, "G");
        text->add(IN[0].x, 10.45f, 6.65f, "IN");
        text->add(OUT[0].x, 10.45f, 6.65f, "OUT");
        static const char* rowNumbers[4] = {"1", "2", "3", "4"};
        for (int i = 0; i < 4; i++)
            text->add(108.12f, G[i].y, 5.25f, rowNumbers[i]);
        for (int i = 0; i < 3; i++)
            text->addLeft(94.70f, LINK[i].y, 5.35f, "linked");

        // ABCD input names.
        static const char* abcdNames[4][3] = {
            {"A1", "A2", "A3"}, {"B1", "B2", "B3"},
            {"C1", "C2", "C3"}, {"D1", "D2", "D3"}};
        for (int col = 0; col < 4; col++) {
            for (int row = 0; row < 3; row++) {
                text->add(cols[col][row].x, cols[col][row].y - 6.20f,
                          6.75f, abcdNames[col][row]);
            }
        }

        // Soft keys beneath the main display. M1/M6 sit above their boxed
        // secondary legends while M2-M5 occupy that secondary baseline.
        text->add(M[0].x, 39.25f, 6.20f, "M1");
        for (int i = 1; i < 5; i++)
            text->add(M[i].x, 42.00f, 6.20f,
                      i == 1 ? "M2" : i == 2 ? "M3" :
                      i == 3 ? "M4" : "M5");
        text->add(M[5].x, 39.25f, 6.20f, "M6");
        text->addBox(M[0].x, 42.30f, 12.10f, 2.55f, 4.85f, "QUICKSAVE");
        text->addBox(M[5].x, 42.30f, 7.70f, 2.55f, 5.05f, "FOCUS");

        // Encoder and lower key legends.
        text->add(4.42f, 83.80f, 5.55f, "fine");
        text->add(8.40f, 87.85f, 5.55f, "coarse");
        text->add(DIAL[1].x, 90.75f, 6.25f, "CANCEL");
        text->addBox(DIAL[2].x, 90.82f, 8.60f, 3.25f, 6.05f, "ZERO");
        text->add(S[0].x, 90.75f, 6.25f, "S1");
        text->add(S[1].x, 90.75f, 6.25f, "S2");
        text->add(S[2].x, 88.30f, 6.25f, "S3");
        text->addBox(S[2].x, 90.82f, 7.70f, 2.55f, 5.05f, "FOCUS");
        text->add(HB[0].x, 104.45f, 6.15f, "ENTER");
        text->addBox(HB[0].x, 106.88f, 9.45f, 2.70f, 5.55f, "COMMIT");
        text->addLeft(65.60f, 106.42f, 6.10f, "UP");
        text->addBox(HB[2].x, 106.72f, 8.05f, 2.75f, 5.85f, "SHIFT");

        // Storage and mode switch fields.
        text->add(8.95f, 104.88f, 6.05f, "STORAGE");
        text->addLeft(12.10f, 109.25f, 5.35f, "user");
        text->addLeft(12.10f, 113.08f, 5.35f, "admin");
        text->addBox(15.75f, 116.90f, 6.85f, 2.70f, 5.15f, "eject");
        text->add(37.80f, 104.88f, 6.05f, "MODE");
        text->addBox(43.45f, 109.25f, 6.20f, 2.70f, 5.05f, "hold");
        text->addLeft(41.00f, 113.08f, 5.35f, "edit");
        text->addLeft(41.00f, 116.90f, 5.35f, "scope");
        text->add(LED_IO.x, 105.45f, 5.35f, "I/O");
        text->add(LED_SAFE.x, 113.08f, 5.35f, "safe");

        // Engineering legends copied from the original panel: bold section
        // names, compact body text, true bidirectional arrows and <= glyphs,
        // with the small DC/AC annotations centered under the voltage spans.
        text->addFooter(13.10f, 124.20f, 7.75f, "G:",
                        NVG_ALIGN_LEFT, -0.08f, 0.92f, 0.18f);
        text->addFooter(16.45f, 124.20f, 6.55f,
                        "0V↔10V | Fs ≤ 96kHz | 12-bit",
                        NVG_ALIGN_LEFT, -0.22f, 0.93f, 0.08f);
        text->addFooter(27.10f, 126.52f, 4.25f, "DC",
                        NVG_ALIGN_CENTER, -0.04f, 0.94f, 0.05f);

        text->addFooter(55.10f, 124.20f, 7.35f, "IN+ABCD:",
                        NVG_ALIGN_LEFT, -0.12f, 0.91f, 0.17f);
        text->addFooter(67.25f, 124.20f, 6.25f,
                        "-10V↔10V | Fs ≤ 60kHz | 16-bit",
                        NVG_ALIGN_LEFT, -0.27f, 0.92f, 0.07f);
        text->addFooter(82.20f, 122.05f, 3.75f, "zoomable",
                        NVG_ALIGN_CENTER, -0.05f, 0.94f, 0.04f);
        text->addFooter(81.80f, 126.52f, 4.25f, "DC",
                        NVG_ALIGN_CENTER, -0.04f, 0.94f, 0.05f);

        text->addFooter(108.80f, 124.20f, 7.35f, "OUT:",
                        NVG_ALIGN_LEFT, -0.10f, 0.91f, 0.17f);
        text->addFooter(115.20f, 124.20f, 6.10f,
                        "-7V↔7V | Fs ≤ 96kHz | 24-bit",
                        NVG_ALIGN_LEFT, -0.27f, 0.92f, 0.07f);
        text->addFooter(126.45f, 126.52f, 4.25f, "AC",
                        NVG_ALIGN_CENTER, -0.04f, 0.94f, 0.05f);

        addChild(text);
    }

    void step() override {
        ER301* m = static_cast<ER301*>(module);
        if (m) {
            // Follow only this module's saved presentation state. The global
            // preference files are defaults for future instances, not a live
            // broadcast to ER-301s that already exist in the patch.
            if (m->panelColor >= 0 && m->panelColor != panelColor)
                applyPanelColor(m->panelColor, false);
            if (m->screenColor >= 0 && m->screenColor != screenColor)
                applyScreenColor(m->screenColor, false);

            m->uiStep();
            serviceHostTextDialog();
            // Consume the newest snapshot published by the independent
            // display pump. Slow Rack UI frames cannot queue stale displays.
            if (m->engine.isReady() && mainDisplay && subDisplay) {
                if (m->engine.getDisplayFrame(nextMain, nextSub)) {
                    if (std::memcmp(nextMain, mainDisplay->mainRaw,
                                    er301::MAIN_BYTES) != 0) {
                        std::memcpy(mainDisplay->mainRaw, nextMain,
                                    er301::MAIN_BYTES);
                        mainDisplay->decodeMain();
                        mainDisplay->haveFrame = true;
                    }
                    if (std::memcmp(nextSub, subDisplay->subRaw,
                                    er301::SUB_BYTES) != 0) {
                        std::memcpy(subDisplay->subRaw, nextSub,
                                    er301::SUB_BYTES);
                        subDisplay->decodeSub();
                        subDisplay->haveFrame = true;
                    }
                }
            }
        }
        ModuleWidget::step();
    }
};

Model* modelER301SoundComputer = createModel<ER301, ER301Widget>("ER-301SoundComputer");
