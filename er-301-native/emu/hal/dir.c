#include <hal/dir.h>
//#define BUILDOPT_VERBOSE
//#define BUILDOPT_DEBUG_LEVEL 10
#include <hal/log.h>
#include <hal/fileops.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct
{
  DIR *handle;
  char *path;
  bool followDirectoryLinks;
} host_dir_t;

static bool hasFinalComponent(const char *path, const char *component)
{
  size_t pathLength = strlen(path);
  while (pathLength > 1 && path[pathLength - 1] == '/')
  {
    pathLength--;
  }

  size_t componentLength = strlen(component);
  if (pathLength < componentLength)
  {
    return false;
  }
  if (strncmp(path + pathLength - componentLength, component,
              componentLength) != 0)
  {
    return false;
  }
  return pathLength == componentLength ||
         path[pathLength - componentLength - 1] == '/';
}

static bool classifyEntry(host_dir_t *d, struct dirent *entry,
                          uint32_t *attributes)
{
#ifdef _WIN32
  /*
   * MinGW's dirent implementation does not expose d_type reliably.
   * Ask the platform file-operations HAL instead.
   */
  size_t pathLength = strlen(d->path);
  size_t nameLength = strlen(entry->d_name);
  char *fullpath = (char *)malloc(pathLength + nameLength + 2);

  if (!fullpath)
  {
    return false;
  }

  memcpy(fullpath, d->path, pathLength);

  if (pathLength > 0 &&
      d->path[pathLength - 1] != '/' &&
      d->path[pathLength - 1] != '\\')
  {
    fullpath[pathLength++] = '/';
  }

  memcpy(fullpath + pathLength, entry->d_name, nameLength + 1);

  uint32_t fileAttributes = 0;
  bool ok = getFileInfo(fullpath, &fileAttributes, 0);

  free(fullpath);

  if (!ok)
  {
    return false;
  }

  if (attributes)
  {
    *attributes = fileAttributes;
  }

  return true;

#else
  bool isDirectory = false;
  bool isRegularFile = false;

  if (entry->d_type == DT_DIR)
  {
    isDirectory = true;
  }
  else if (entry->d_type == DT_REG)
  {
    isRegularFile = true;
  }
  else if (entry->d_type == DT_UNKNOWN ||
           (entry->d_type == DT_LNK && d->followDirectoryLinks))
  {
    // ER-301 Sound Computer exposes each approved external library as one directory symlink
    // directly inside "External Libraries". Follow links only while listing
    // that one container. Once inside the chosen external folder, nested
    // symlinks retain the emulator's original skip behavior, avoiding loops.
    size_t pathLength = strlen(d->path);
    size_t nameLength = strlen(entry->d_name);
    char *fullpath = (char *)malloc(pathLength + nameLength + 2);
    if (fullpath)
    {
      memcpy(fullpath, d->path, pathLength);
      if (pathLength > 0 && d->path[pathLength - 1] != '/')
      {
        fullpath[pathLength++] = '/';
      }
      memcpy(fullpath + pathLength, entry->d_name, nameLength + 1);

      struct stat st;
      if (stat(fullpath, &st) == 0)
      {
        isDirectory = S_ISDIR(st.st_mode);
        isRegularFile = S_ISREG(st.st_mode);
      }
      free(fullpath);
    }
  }

  if (!isDirectory && !isRegularFile)
  {
    return false;
  }

  if (attributes)
  {
    *attributes = isDirectory ? FILEOPS_DIR : 0;
  }
  return true;
#endif
}

dir_t Dir_open(const char *path)
{
  logDebug(1, path);
  DIR *handle = opendir(path);
  if (handle == 0)
  {
    return 0;
  }

  host_dir_t *d = (host_dir_t *)malloc(sizeof(host_dir_t));
  if (d == 0)
  {
    closedir(handle);
    return 0;
  }
  d->path = strdup(path);
  if (d->path == 0)
  {
    closedir(handle);
    free(d);
    return 0;
  }
  d->handle = handle;
  d->followDirectoryLinks = hasFinalComponent(path, "External Libraries");
  return (dir_t)d;
}

void Dir_close(dir_t dir)
{
  host_dir_t *d = (host_dir_t *)dir;
  logAssert(d);
  closedir(d->handle);
  free(d->path);
  free(d);
}

bool Dir_read(dir_t dir, char **filename, uint32_t *attributes)
{
  host_dir_t *d = (host_dir_t *)dir;
  logAssert(d);

  struct dirent *entry = readdir(d->handle);

  while (entry)
  {
    // Skip the . and .. entries.
    if ((strcmp(entry->d_name, ".") == 0) ||
        (strcmp(entry->d_name, "..") == 0))
    {
      logDebug(1, "Skipping %s", entry->d_name);
      entry = readdir(d->handle);
      continue;
    }

    uint32_t entryAttributes = 0;
    if (!classifyEntry(d, entry, &entryAttributes))
    {
#ifdef _WIN32
      logDebug(1, "Skipping %s", entry->d_name);
#else
      logDebug(1, "Skipping %s (d_type = 0x%x)", entry->d_name,
               entry->d_type);
#endif
      entry = readdir(d->handle);
      continue;
    }

    if (filename)
    {
      *filename = entry->d_name;
    }
    if (attributes)
    {
      *attributes = entryAttributes;
    }
    return true;
  }

  return false;
}
