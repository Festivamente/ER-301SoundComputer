#pragma once

#include <functional>
#include <string>

namespace emu
{
  typedef std::function<bool(const std::string &)> NativeSymbolResolver;

  // Rewrites a Darwin ER-301 package image so symbols exported by the current
  // isolated ER-301 Sound Computer engine bind explicitly to that image instead of the
  // process-wide flat namespace. The input is never modified in place.
  bool patchNativePackageMachO(const std::string &sourcePath,
                               const std::string &destinationPath,
                               const std::string &engineImagePath,
                               const NativeSymbolResolver &engineDefines,
                               std::string &error);
}
