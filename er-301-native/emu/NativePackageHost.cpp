#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "NativePackageMachO.h"

#include <od/objects/Inlet.h>
#include <od/objects/Option.h>
#include <od/objects/Outlet.h>
#include <od/objects/Parameter.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <string>

#if defined(__APPLE__)
#include <dlfcn.h>
#include <fcntl.h>
#include <libgen.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace
{
  static std::string foreignString(const void *object)
  {
    if (!object)
      return std::string();
    const unsigned char *bytes = static_cast<const unsigned char *>(object);
    const char *data = *reinterpret_cast<const char *const *>(bytes);
    const std::size_t length = *reinterpret_cast<const std::size_t *>(bytes + 8);
    if (!data || length > 1024 * 1024)
      return std::string();
    return std::string(data, length);
  }

#if defined(__APPLE__)
  static uint64_t fnv1a(const std::string &text, uint64_t value = 1469598103934665603ULL)
  {
    for (std::size_t i = 0; i < text.size(); ++i)
    {
      value ^= static_cast<unsigned char>(text[i]);
      value *= 1099511628211ULL;
    }
    return value;
  }

  static bool ensureDirectory(const std::string &path, std::string &error)
  {
    if (path.empty())
    {
      error = "Empty native-package cache path.";
      return false;
    }
    struct stat st;
    if (::stat(path.c_str(), &st) == 0)
    {
      if (S_ISDIR(st.st_mode))
        return true;
      error = "Native-package cache path exists but is not a directory: " + path;
      return false;
    }
    if (errno != ENOENT)
    {
      error = "Could not inspect native-package cache path: " +
              std::string(std::strerror(errno));
      return false;
    }
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0)
    {
      if (!ensureDirectory(path.substr(0, slash), error))
        return false;
    }
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
    {
      error = "Could not create native-package cache directory: " +
              std::string(std::strerror(errno));
      return false;
    }
    return true;
  }

  static std::string canonicalPath(const char *path)
  {
    if (!path)
      return std::string();
    char *resolved = ::realpath(path, 0);
    if (!resolved)
      return std::string(path);
    std::string result(resolved);
    std::free(resolved);
    return result;
  }

  static std::string currentEngineImage(std::string &error)
  {
    Dl_info info;
    if (::dladdr(reinterpret_cast<const void *>(&currentEngineImage), &info) == 0 ||
        !info.dli_fname)
    {
      error = "Could not locate the current ER-301 Sound Computer engine image.";
      return std::string();
    }
    // Keep the exact dyld identity. A realpath-normalized spelling may not
    // match the path under which an already-loaded image was registered.
    return std::string(info.dli_fname);
  }

  static bool adHocSign(const std::string &path, std::string &error)
  {
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0)
    {
      error = "Could not initialize ad-hoc signing process.";
      return false;
    }
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                     O_WRONLY, 0);

    const char *argv[] = {"codesign", "--force", "--sign", "-",
                          "--timestamp=none", path.c_str(), 0};
    pid_t pid = 0;
    const int spawnResult = posix_spawn(&pid, "/usr/bin/codesign", &actions,
                                        0, const_cast<char *const *>(argv),
                                        environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawnResult != 0)
    {
      error = "Could not launch /usr/bin/codesign for native package: " +
              std::string(std::strerror(spawnResult));
      return false;
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0)
    {
      if (errno == EINTR)
        continue;
      error = "Could not wait for native-package signing: " +
              std::string(std::strerror(errno));
      return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
      error = "macOS could not ad-hoc sign the isolated native package image.";
      return false;
    }
    return true;
  }

  static std::string basenameOf(const std::string &path)
  {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }

  static std::string directoryOf(const std::string &path)
  {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
      return ".";
    if (slash == 0)
      return "/";
    return path.substr(0, slash);
  }

#endif
}

extern "C"
{
  __attribute__((used, visibility("default")))
  void soundcomputer_compat_inlet_ctor(od::Inlet *self, const void *name)
  {
    new (self) od::Inlet(foreignString(name));
  }

  __attribute__((used, visibility("default")))
  void soundcomputer_compat_inlet_indexed_ctor(od::Inlet *self, const void *name,
                                        uint32_t index)
  {
    new (self) od::Inlet(foreignString(name), index);
  }

  __attribute__((used, visibility("default")))
  void soundcomputer_compat_outlet_ctor(od::Outlet *self, const void *name)
  {
    new (self) od::Outlet(foreignString(name));
  }

  __attribute__((used, visibility("default")))
  void soundcomputer_compat_outlet_indexed_ctor(od::Outlet *self, const void *name,
                                         uint32_t index)
  {
    new (self) od::Outlet(foreignString(name), index);
  }

  __attribute__((used, visibility("default")))
  void soundcomputer_compat_parameter_ctor(od::Parameter *self, const void *name,
                                    float initialValue)
  {
    new (self) od::Parameter(foreignString(name), initialValue);
  }

  __attribute__((used, visibility("default")))
  void soundcomputer_compat_option_ctor(od::Option *self, const void *name)
  {
    new (self) od::Option(foreignString(name));
  }

  __attribute__((used, visibility("default")))
  void soundcomputer_compat_option_value_ctor(od::Option *self, const void *name,
                                       int value)
  {
    new (self) od::Option(foreignString(name), value);
  }

  // Called by Lua's package loader before dlopen(). The returned image is a
  // private, ad-hoc-signed copy whose ER-301 and Lua symbols bind to this
  // exact engine image rather than another live ER-301 Sound Computer module.
  __attribute__((used, visibility("default")))
  int soundcomputer_prepare_native_package(const char *sourcePath, char *outputPath,
                                    std::size_t outputCapacity,
                                    char *errorText,
                                    std::size_t errorCapacity)
  {
    std::string error;
#if defined(__APPLE__)
    if (!sourcePath || !outputPath || outputCapacity == 0)
    {
      error = "Invalid native-package preparation request.";
    }
    else
    {
      const std::string source = canonicalPath(sourcePath);
      const std::string engine = currentEngineImage(error);
      if (!engine.empty())
      {
        void *engineHandle = ::dlopen(engine.c_str(), RTLD_NOW | RTLD_NOLOAD
#ifdef RTLD_FIRST
                                     | RTLD_FIRST
#endif
        );
        if (!engineHandle)
        {
          const char *message = ::dlerror();
          error = "Could not obtain the current ER-301 Sound Computer engine handle: ";
          error += message ? message : "unknown dlopen error";
        }
        else
        {
          const std::string cacheDirectory =
              directoryOf(source) + "/.er-301soundcomputer-native";
          if (ensureDirectory(cacheDirectory, error))
          {
            uint64_t hash = fnv1a(source);
            hash = fnv1a(engine, hash);
            std::ostringstream name;
            name << basenameOf(source) << "." << std::hex << hash << ".so";
            const std::string destination = cacheDirectory + "/" + name.str();

            struct stat sourceStat;
            struct stat engineStat;
            struct stat destinationStat;
            const bool haveSourceStat = ::stat(source.c_str(), &sourceStat) == 0;
            const bool haveEngineStat = ::stat(engine.c_str(), &engineStat) == 0;
            const bool haveDestination =
                ::stat(destination.c_str(), &destinationStat) == 0;
            const bool needsBuild =
                !haveDestination ||
                (haveSourceStat && destinationStat.st_mtime < sourceStat.st_mtime) ||
                (haveEngineStat && destinationStat.st_mtime < engineStat.st_mtime);

            if (needsBuild)
            {
              const emu::NativeSymbolResolver resolver =
                  [engineHandle](const std::string &symbol) -> bool {
                    // Mach-O bind streams include the object-file leading
                    // underscore. dlsym() expects the source-level spelling.
                    const char *query = symbol.c_str();
                    if (!symbol.empty() && symbol[0] == '_')
                      ++query;
                    ::dlerror();
                    void *address = ::dlsym(engineHandle, query);
                    return address != 0 && ::dlerror() == 0;
                  };
              if (emu::patchNativePackageMachO(source, destination, engine,
                                               resolver, error) &&
                  !adHocSign(destination, error))
                std::remove(destination.c_str());
            }

            if (error.empty())
            {
              if (destination.size() + 1 > outputCapacity)
                error = "Prepared native-package path is too long.";
              else
              {
                std::memcpy(outputPath, destination.c_str(),
                            destination.size() + 1);
                ::dlclose(engineHandle);
                if (errorText && errorCapacity)
                  errorText[0] = 0;
                return 1;
              }
            }
          }
          ::dlclose(engineHandle);
        }
      }
    }
#else
    if (sourcePath && outputPath && outputCapacity > std::strlen(sourcePath))
    {
      std::strcpy(outputPath, sourcePath);
      if (errorText && errorCapacity)
        errorText[0] = 0;
      return 1;
    }
    error = "Native package preparation is unavailable on this platform.";
#endif

    if (errorText && errorCapacity)
    {
      const std::size_t length = std::min(error.size(), errorCapacity - 1);
      std::memcpy(errorText, error.data(), length);
      errorText[length] = 0;
    }
    return 0;
  }

  // Keep compatibility entry points visible even under aggressive dead-strip.
  __attribute__((used, visibility("default")))
  void *soundcomputer_native_package_compatibility_exports[] = {
      reinterpret_cast<void *>(&soundcomputer_compat_inlet_ctor),
      reinterpret_cast<void *>(&soundcomputer_compat_inlet_indexed_ctor),
      reinterpret_cast<void *>(&soundcomputer_compat_outlet_ctor),
      reinterpret_cast<void *>(&soundcomputer_compat_outlet_indexed_ctor),
      reinterpret_cast<void *>(&soundcomputer_compat_parameter_ctor),
      reinterpret_cast<void *>(&soundcomputer_compat_option_ctor),
      reinterpret_cast<void *>(&soundcomputer_compat_option_value_ctor),
      reinterpret_cast<void *>(&soundcomputer_prepare_native_package)};
}
