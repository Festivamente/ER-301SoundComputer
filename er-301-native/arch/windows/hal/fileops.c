#include <hal/fileops.h>
#include <windows.h>
#include <stdint.h>

bool createDirectory(const char *path)
{
  if (CreateDirectoryA(path, NULL))
    return true;

  if (GetLastError() == ERROR_ALREADY_EXISTS)
  {
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
  }

  return false;
}

bool deleteDirectory(const char *path)
{
  return RemoveDirectoryA(path) != 0;
}

bool moveDirectory(const char *fromPath, const char *toPath)
{
  return MoveFileExA(fromPath, toPath, MOVEFILE_COPY_ALLOWED) != 0;
}

bool deleteFile(const char *path)
{
  return DeleteFileA(path) != 0;
}

bool pathExists(const char *path)
{
  return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

bool getFileInfo(const char *path, uint32_t *attributes, uint64_t *sizeInBytes)
{
  WIN32_FILE_ATTRIBUTE_DATA info;

  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info))
    return false;

  if (attributes)
  {
    uint32_t result = 0;

    if (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
      result |= FILEOPS_RDO;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)
      result |= FILEOPS_HID;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)
      result |= FILEOPS_SYS;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      result |= FILEOPS_DIR;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)
      result |= FILEOPS_ARC;

    *attributes = result;
  }

  if (sizeInBytes)
  {
    ULARGE_INTEGER size;
    size.HighPart = info.nFileSizeHigh;
    size.LowPart = info.nFileSizeLow;
    *sizeInBytes = (uint64_t)size.QuadPart;
  }

  return true;
}

bool isDirectory(const char *path)
{
  DWORD attr = GetFileAttributesA(path);

  return attr != INVALID_FILE_ATTRIBUTES &&
         (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool moveFile(const char *fromPath, const char *toPath, bool overwrite)
{
  DWORD flags = MOVEFILE_COPY_ALLOWED;

  if (overwrite)
    flags |= MOVEFILE_REPLACE_EXISTING;

  return MoveFileExA(fromPath, toPath, flags) != 0;
}

bool copyFile(const char *fromPath, const char *toPath, bool overwrite)
{
  return CopyFileA(fromPath, toPath, overwrite ? FALSE : TRUE) != 0;
}

int diskFreeSpaceMB(const char *path)
{
  ULARGE_INTEGER freeBytes;

  if (!GetDiskFreeSpaceExA(path, &freeBytes, NULL, NULL))
    return 0;

  return (int)(freeBytes.QuadPart / 1024ULL / 1024ULL);
}

int diskTotalSpaceMB(const char *path)
{
  ULARGE_INTEGER totalBytes;

  if (!GetDiskFreeSpaceExA(path, NULL, &totalBytes, NULL))
    return 0;

  return (int)(totalBytes.QuadPart / 1024ULL / 1024ULL);
}
