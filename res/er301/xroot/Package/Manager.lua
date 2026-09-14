--[[ Package Manager

Package archives (*.pkg) are found in 0:/ER-301/packages.
Package contents are installed in 0:/ER-301/<version>/libs. Or 1:/ER-301/<version>/libs?
Upgrading a package removes other
Installed packages are loaded when init() is called (or on the first card mount afterwards).
Packages are also loaded when installed? Or require a reboot?

Usage in code:
local Loopers = require "loopers-1.0.2"

]] --
local Card = require "Card"
local FS = require "Card.FileSystem"
local Path = require "Path"
local Signal = require "Signal"
local Busy = require "Busy"
local Package = require "Package"
local Utils = require "Utils"
local Persist = require "Persist"
local SemanticVersion = require "SemanticVersion"

local loaded = {}
local packageCache = {}
local packageCacheIsStale = false
local rebootNeeded = false

local function isRebootNeeded()
  return rebootNeeded
end

local function invalidatePackageCache()
  packageCacheIsStale = true
end

-- Move this into the Interface module?
local function refreshPackageCache()
  Busy.start("Refreshing packages...")
  local keep = {}
  -- Refresh installed packages.
  local installed = Persist.getRearCardValue("packages", "installed") or {}
  for k, v in pairs(installed) do
    if packageCache[k] == nil then
      packageCache[k] = Package(v)
    end
    packageCache[k].installed = true
    keep[k] = true
  end
  if Card.mounted() then
    -- Refresh packages with package archives.
    local repoPath = FS.getRoot("packages")
    for filename in dir(repoPath) do
      if FS.isType("package", filename) then
        Busy.status("Refreshing repository: " .. filename)
        local package = Package(filename)
        if package.id then
          keep[package.id] = true
          if installed[package.id] == nil then
            packageCache[package.id] = package
          end
        end
      end
    end
  end
  -- Remove any packages that are not installed and don't have an archive.
  for k, _ in pairs(packageCache) do
    if not keep[k] then
      packageCache[k] = nil
    end
  end
  Busy.stop()
  packageCacheIsStale = false
end

local function getPackages()
  if packageCacheIsStale then
    refreshPackageCache()
  end
  return packageCache
end

local function getPackage(id)
  if packageCacheIsStale then
    refreshPackageCache()
  end
  return packageCache[id]
end

local function findCompatiblePackage(dep, installedOnly)
  local C = {}
  -- Gather candidate packages that satisfy the dependencies.
  if installedOnly then
    for _, package in pairs(getPackages()) do
      if package.installed and package:satisfies(dep) then
        C[#C + 1] = package
      end
    end
  else
    for _, package in pairs(getPackages()) do
      if package:satisfies(dep) then
        C[#C + 1] = package
      end
    end
  end
  -- Choose the candidate with the highest version.
  local best
  local vMax = SemanticVersion('0-0')
  for _, package in ipairs(C) do
    local v = SemanticVersion(package:getVersion())
    if v > vMax then
      vMax = v
      best = package
    end
  end
  return best
end

local function dependencyToPrettyString(dep)
  if type(dep) == "table" then
    if #dep == 1 then
      return dep[1]
    elseif #dep == 2 then
      return string.format("%s[%s]", dep[1], dep[2])
    elseif #dep == 3 then
      return string.format("%s[%s,%s]", dep[1], dep[2], dep[3])
    end
  elseif type(dep) == "string" then
    return dep
  end
end

local function unrequire(moduleName)
  for k, _ in pairs(package.loaded) do
    if Utils.startsWith(k, moduleName) then
      app.logInfo("Unrequire: %s", k)
      package.loaded[k] = nil
    end
  end
end

local function unload(package)
  if loaded[package.id] then
    app.logInfo("Unloading package: %s", package.id)
    loaded[package.id] = nil
    package:onUnload()
    unrequire(package:getName())
    app.collectgarbage()
    rebootNeeded = true
  end
end

local function packageNameFromId(id)
  return id:match("^(.*)-.*-.*$") or id:match("^(.*)-.*$") or id
end

local function installedPackageIdWithName(installed, name)
  for id, _ in pairs(installed) do
    if packageNameFromId(id) == name then
      return id
    end
  end
end

local function removeInstallationFolder(package)
  local installFolder = package:getInstallationPath()
  if not Path.exists(installFolder) then
    return true
  end

  app.logInfo("Removing package installation folder: %s", installFolder)
  if not Path.recursiveDelete(installFolder) or Path.exists(installFolder) then
    local msg = string.format(
                    "%s: Could not completely remove the installed package folder. Restart Rack and try again.",
                    package.id)
    app.logError(msg)
    return false, msg
  end
  return true
end

local function uninstall(package)
  if package.installed then
    unload(package)
    app.logInfo("Uninstalling package: %s", package.id)
    Busy.start("Uninstalling %s", package.id)

    local removed, msg = removeInstallationFolder(package)
    if not removed then
      Busy.stop()
      return false, msg
    end

    local installed = Persist.getRearCardValue("packages", "installed") or {}
    installed[package.id] = nil
    Persist.setRearCardValue("packages", "installed", installed)

    package.installed = false
    packageCacheIsStale = true

    local UnitFactory = require "Unit.Factory"
    UnitFactory.reloadLibraries()
    Busy.stop()
    return true
  end
  return false
end

local function load(package)
  if loaded[package.id] then
    return true
  end

  app.logInfo("Loading package: %s", package.id)

  -- Make sure all dependencies are loaded first
  local deps = package:getDependencies()
  if deps then
    for _, dep in ipairs(deps) do
      local otherPackage = findCompatiblePackage(dep, true)
      if otherPackage then
        if not load(otherPackage) then
          app.logError("%s: failed to load dependency, %s.", package.id,
                       otherPackage.id)
          return false
        end
      else
        app.logError("%s: required package (%s) not found.", package.id,
                     dependencyToPrettyString(dep))
        return false
      end
    end
  end

  package:onLoad()
  loaded[package.id] = package
  return true
end

local function delete(package)
  if package.installed then
    local removed, msg = uninstall(package)
    if not removed then
      return false, msg
    end
  end
  packageCache[package.id] = nil
  packageCacheIsStale = true
  local path = package:getArchivePath()
  if Path.exists(path) and not app.deleteFile(path) then
    local msg = string.format("%s: Could not delete package archive.", package.id)
    app.logError(msg)
    return false, msg
  end
  return true
end

local function install(package)
  if package.installed then
    -- already installed
    return true
  end

  local installFolder = package:getInstallationPath()
  if Path.exists(installFolder) then
    local installed = Persist.getRearCardValue("packages", "installed") or {}
    local installedId = installedPackageIdWithName(installed, package:getName())
    if installedId then
      local msg = string.format("%s: Another version is already installed (%s).",
                                package.id, installedId)
      app.logInfo(msg)
      return false, msg
    end

    -- A folder without a matching installed-package record is debris from an
    -- interrupted or older uninstall. Remove it now instead of permanently
    -- blocking this package name.
    app.logWarn("%s: Removing stale unregistered installation folder.",
                package.id)
    local removed, msg = removeInstallationFolder(package)
    if not removed then
      return false, msg
    end
  end

  app.logInfo("Installing package: %s", package.id)
  Busy.start("Installing %s", package.id)

  -- Make sure dependencies are installed first
  -- TODO: side-by-side installs are not allowed
  local deps = package:getDependencies()
  if deps then
    for _, dep in ipairs(deps) do
      local otherPackage = findCompatiblePackage(dep)
      if otherPackage then
        if not install(otherPackage) then
          local msg = string.format("%s: failed to install dependency, %s.",
                                    package.id, otherPackage.name)
          app.logInfo(msg)
          Busy.stop()
          return false, msg
        end
      else
        local msg = string.format("%s: required package (%s) not found.",
                                  package.id, dependencyToPrettyString(dep))
        app.logInfo(msg)
        Busy.stop()
        return false, msg
      end
    end
  end

  local archive = app.ZipArchiveReader()
  local pathToArchive = package:getArchivePath()
  if not archive:open(pathToArchive) then
    local msg = string.format("Failed to open package archive: %s",
                              pathToArchive)
    app.logInfo(msg)
    Busy.stop()
    return false, msg
  end

  -- Extract files to system library folder
  local n = archive:getFileCount()
  for i = 1, n do
    local filename = archive:getFilename(i - 1)
    local path = Path.join(installFolder, filename)
    Path.createAll(path, true)
    if Path.isDirectory(path:gsub("/$", "")) then
      -- app.logInfo("Skipping folder: %s", path)
    else
      Busy.status("Installing %s: %s", package.id, Path.getFilename(filename))
      -- app.logInfo("Extracting %s.", path)
      if not archive:extract(filename, path) then
        local msg = string.format("Failed to extract %s from %s.", filename,
                                  package.id)
        app.logInfo(msg)
        archive:close()
        Path.recursiveDelete(installFolder)
        Busy.stop()
        return false, msg
      end
    end
  end
  archive:close()
  package.installed = true

  local installed = Persist.getRearCardValue("packages", "installed") or {}
  installed[package.id] = package.filename
  Persist.setRearCardValue("packages", "installed", installed)
  load(package)

  local UnitFactory = require "Unit.Factory"
  UnitFactory.reloadLibraries()
  Busy.stop()
  return true
end

local function loadInstalledPackages()
  for id, package in pairs(getPackages()) do
    if package.installed and loaded[package.id] == nil then
      Busy.start("Loading package: " .. id)
      load(package)
      Busy.stop()
    end
  end
  local UnitFactory = require "Unit.Factory"
  UnitFactory.reloadLibraries()
end

local function reset()
  Busy.start("Uninstalling all packages...")
  for k, v in pairs(loaded) do
    unload(v)
  end
  Path.recursiveDelete(FS.getRoot("libs"))
  Path.recursiveDelete(FS.getRoot("package-configs"))
  Persist.deleteRearCardDatabase("packages")

  Path.createAll(FS.getRoot("libs"))
  Path.createAll(FS.getRoot("package-configs"))
  rebootNeeded = true
  packageCacheIsStale = true
  Busy.stop()
end

local function installUpdates()
  -- Updates are packages found in the root of the rear SD card.
  local packages = Utils.shallowCopy(getPackages())
  local updated = 0
  for filename in dir(app.roots.rear) do
    if FS.isType("package", filename) then
      local update = Package(filename)
      local name = update:getName()
      app.logInfo("Installing update: %s", update.id)
      local wasInstalled = false
      local wasPresent = false
      -- remove all packages with the same name
      for id, package in pairs(packages) do
        if package:getName() == name then
          wasPresent = true
          if package.installed then
            wasInstalled = true
          end
          app.logInfo("Removing package %s", id)
          delete(package)
        end
      end
      local srcPath = Path.join(app.roots.rear, filename)
      local dstPath = Path.join(FS.getRoot("packages"), filename)
      if not app.copyFile(srcPath, dstPath, true) then
        app.logWarn("Failed to copy %s to %s.", srcPath, dstPath)
      else
        app.logInfo("Copied %s to %s.", srcPath, dstPath)
        app.deleteFile(srcPath)
        updated = updated + 1
        if wasInstalled or not wasPresent then
          install(update)
        end
      end
    end
  end
  packageCacheIsStale = updated > 0
end

local function cardMounted()
  packageCacheIsStale = true
end

local function cardEjected()
  packageCacheIsStale = true
end

-- A file packer is a function that will be called as follows:
-- myFilePacker(archive, srcPath, dstPath)
-- archive: an open ZipArchiveWriter object
-- srcPath: path to file to be packaged
-- dstPath: destination path in the archive
local function registerFilePackager(ext, func)
  local PackageWriter = require "Package.Writer"
  PackageWriter.packagers[ext] = func
end

-- The VCV port ships the standard Core archive on the emulated front card.
-- Install it once on a new VCV card, then remember that the bootstrap has
-- happened. The marker is intentionally stored on the front card rather than
-- in the package database: if the user later uninstalls Core, it stays
-- uninstalled instead of being silently restored at every boot.
local function installBundledCoreOnce()
  local markerDb = "ER301SoundComputer"
  local markerKey = "bundledCoreBootstrapComplete"
  if Persist.getFrontCardValue(markerDb, markerKey) then
    return
  end

  -- Respect an existing Core installation, including one made manually before
  -- this bootstrap behavior was introduced.
  for _, candidate in pairs(getPackages()) do
    if candidate.installed and candidate:getName() == "core" then
      Persist.setFrontCardValue(markerDb, markerKey, true)
      return
    end
  end

  local bundled = getPackage("core-0.7.0-dev1.8")
  if bundled and install(bundled) then
    Persist.setFrontCardValue(markerDb, markerKey, true)
  else
    app.logWarn("Bundled Core package could not be installed automatically.")
  end
end

local function init()
  package.cpath = string.format("%s/?.%s", FS.getRoot("libs"),
                                FS.getExt("linkable"))
  Signal.register("cardMounted", cardMounted)
  Signal.register("cardEjected", cardEjected)
  packageCacheIsStale = true
  local UnitFactory = require "Unit.Factory"
  UnitFactory.init()
  installUpdates()
  installBundledCoreOnce()
  loadInstalledPackages()
end

return {
  init = init,
  getPackages = getPackages,
  getPackage = getPackage,
  registerFilePackager = registerFilePackager,
  install = install,
  uninstall = uninstall,
  delete = delete,
  reset = reset,
  isRebootNeeded = isRebootNeeded,
  invalidatePackageCache = invalidatePackageCache
}
