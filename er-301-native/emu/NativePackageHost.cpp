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
#include <iterator>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
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

#if defined(_WIN32) || defined(__APPLE__)
  static uint64_t fnv1a(const std::string &text, uint64_t value = 1469598103934665603ULL)
  {
    for (std::size_t i = 0; i < text.size(); ++i)
    {
      value ^= static_cast<unsigned char>(text[i]);
      value *= 1099511628211ULL;
    }
    return value;
  }

  static std::string basenameOf(const std::string &path)
  {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }

  static std::string directoryOf(const std::string &path)
  {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos)
      return ".";
    if (slash == 0)
      return path.substr(0, 1);
    return path.substr(0, slash);
  }

#if defined(_WIN32)
  static const char windowsEngineImageAnchor = 0;

  static std::string windowsError(const char *prefix, DWORD code)
  {
    std::ostringstream message;
    message << prefix << " (Windows error " << code << ").";
    return message.str();
  }

  static bool ensureDirectory(const std::string &path, std::string &error)
  {
    const DWORD attributes = ::GetFileAttributesA(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
      if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        return true;
      error = "Native-package cache path exists but is not a directory: " + path;
      return false;
    }

    const DWORD inspectError = ::GetLastError();
    if (inspectError != ERROR_FILE_NOT_FOUND &&
        inspectError != ERROR_PATH_NOT_FOUND)
    {
      error = windowsError("Could not inspect native-package cache path", inspectError);
      return false;
    }

    const std::string parent = directoryOf(path);
    if (parent != path && parent != "." && !ensureDirectory(parent, error))
      return false;

    if (!::CreateDirectoryA(path.c_str(), 0))
    {
      const DWORD createError = ::GetLastError();
      if (createError != ERROR_ALREADY_EXISTS)
      {
        error = windowsError("Could not create native-package cache directory", createError);
        return false;
      }
    }
    return true;
  }

  static std::string currentEngineImage(std::string &error)
  {
    HMODULE module = 0;
    if (!::GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&windowsEngineImageAnchor), &module))
    {
      error = windowsError("Could not locate the current ER-301 Sound Computer engine image",
                           ::GetLastError());
      return std::string();
    }

    std::vector<char> path(32768, 0);
    const DWORD length = ::GetModuleFileNameA(
        module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || static_cast<std::size_t>(length) >= path.size())
    {
      error = windowsError("Could not read the current ER-301 Sound Computer engine path",
                           ::GetLastError());
      return std::string();
    }
    return std::string(path.data(), length);
  }

  static bool patchWindowsPackageHost(const std::string &source,
                                      const std::string &destination,
                                      const std::string &engineBasename,
                                      std::string &error)
  {
    static const char neutralHost[] = "er301host.dll";
    const std::size_t neutralLength = sizeof(neutralHost) - 1;
    if (engineBasename.size() > neutralLength)
    {
      error = "Windows ER-301 engine basename is too long for the neutral package-host ABI: " +
              engineBasename;
      return false;
    }

    std::ifstream input(source.c_str(), std::ios::binary);
    if (!input)
    {
      error = "Could not open Windows native package for host binding: " + source;
      return false;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    if (bytes.empty())
    {
      error = "Windows native package is empty: " + source;
      return false;
    }

    for (std::size_t i = 0; i + neutralLength < bytes.size(); ++i)
    {
      if (std::memcmp(bytes.data() + i, neutralHost, neutralLength) == 0 &&
          bytes[i + neutralLength] == '\0')
      {
        std::memset(bytes.data() + i, 0, neutralLength);
        std::memcpy(bytes.data() + i, engineBasename.data(),
                    engineBasename.size());
        i += neutralLength - 1;
      }
    }

    // A native module that does not use the ER-301 host ABI still remains a
    // valid Windows package. Preserve it unchanged in the private cache.
    const std::string temporary = destination + ".tmp";
    {
      std::ofstream output(temporary.c_str(),
                           std::ios::binary | std::ios::trunc);
      if (!output)
      {
        error = "Could not create prepared Windows native package: " + temporary;
        return false;
      }
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      if (!output)
      {
        error = "Could not write prepared Windows native package: " + temporary;
        output.close();
        ::DeleteFileA(temporary.c_str());
        return false;
      }
    }

    if (!::MoveFileExA(temporary.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
      const DWORD moveError = ::GetLastError();
      ::DeleteFileA(temporary.c_str());
      error = windowsError("Could not install prepared Windows native package", moveError);
      return false;
    }
    return true;
  }

#elif defined(__APPLE__)
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

#endif
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

  // Called by Lua's package loader before the platform dynamic loader. The
  // returned image is rebound to this exact ER-301 engine image: Mach-O uses
  // explicit install-name rebinding on macOS, while Windows rewrites the
  // neutral er301host.dll import to the current private engine basename.
  __attribute__((used, visibility("default")))
  int soundcomputer_prepare_native_package(const char *sourcePath, char *outputPath,
                                    std::size_t outputCapacity,
                                    char *errorText,
                                    std::size_t errorCapacity)
  {
    std::string error;
#if defined(_WIN32)
    if (!sourcePath || !outputPath || outputCapacity == 0)
    {
      error = "Invalid native-package preparation request.";
    }
    else
    {
      const std::string source(sourcePath);
      const std::string engine = currentEngineImage(error);
      if (!engine.empty())
      {
        const std::string engineBasename = basenameOf(engine);
        static const std::size_t neutralHostLength = sizeof("er301host.dll") - 1;
        if (engineBasename.size() > neutralHostLength)
        {
          error = "Current Windows ER-301 engine filename cannot bind the neutral package ABI: " +
                  engineBasename;
        }
        else
        {
          WIN32_FILE_ATTRIBUTE_DATA sourceInfo;
          uint64_t hash = fnv1a(source);
          hash = fnv1a(engine, hash);
          if (::GetFileAttributesExA(source.c_str(), GetFileExInfoStandard,
                                     &sourceInfo))
          {
            std::ostringstream stamp;
            stamp << sourceInfo.nFileSizeHigh << ':' << sourceInfo.nFileSizeLow
                  << ':' << sourceInfo.ftLastWriteTime.dwHighDateTime << ':'
                  << sourceInfo.ftLastWriteTime.dwLowDateTime;
            hash = fnv1a(stamp.str(), hash);
          }

          const std::string cacheDirectory =
              directoryOf(source) + "/.er-301soundcomputer-native";
          if (ensureDirectory(cacheDirectory, error))
          {
            std::ostringstream name;
            name << basenameOf(source) << "." << std::hex << hash << ".dll";
            const std::string destination = cacheDirectory + "/" + name.str();

            const DWORD destinationAttributes =
                ::GetFileAttributesA(destination.c_str());
            if (destinationAttributes == INVALID_FILE_ATTRIBUTES &&
                !patchWindowsPackageHost(source, destination,
                                         engineBasename, error))
            {
              // patchWindowsPackageHost supplied the detailed error.
            }
            else if (error.empty())
            {
              if (destination.size() + 1 > outputCapacity)
              {
                error = "Prepared native-package path is too long.";
              }
              else
              {
                std::memcpy(outputPath, destination.c_str(),
                            destination.size() + 1);
                if (errorText && errorCapacity)
                  errorText[0] = 0;
                return 1;
              }
            }
          }
        }
      }
    }
#elif defined(__APPLE__)
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
