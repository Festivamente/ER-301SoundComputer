#include <hal/heap.h>
#include <hal/log.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <malloc.h>

typedef struct
{
  uintptr_t unusedMemoryStart;
  uint32_t unusedMemorySize;
} Local;

static Local local;

uintptr_t Heap_getUnusedMemoryStart()
{
  return local.unusedMemoryStart;
}

uint32_t Heap_getUnusedMemorySize()
{
  return local.unusedMemorySize;
}

void Heap_print(void)
{
  logInfo("Heap_print: not implemented.");
}

int Heap_getSize(int units)
{
  return (int)(0 / units);
}

int Heap_getFreeSize(int units)
{
  return (int)(0 / units);
}

void Heap_init()
{
  Heap_deinit();
  local.unusedMemorySize = 487 * 1024 * 1024;
  local.unusedMemoryStart = (uintptr_t)malloc(local.unusedMemorySize);
}

void Heap_deinit()
{
  if (local.unusedMemoryStart)
  {
    free((void *)local.unusedMemoryStart);
    local.unusedMemoryStart = 0;
  }

  local.unusedMemorySize = 0;
}

void *Heap_memalign(size_t align, size_t size)
{
  if (align < sizeof(void *))
    align = sizeof(void *);

  return _aligned_malloc(size, align);
}

void *Heap_malloc(size_t size)
{
  return _aligned_malloc(size, 16);
}

void *Heap_calloc(size_t nmemb, size_t size)
{
  if (nmemb && size > SIZE_MAX / nmemb)
    return NULL;

  size_t total = nmemb * size;
  void *ptr = _aligned_malloc(total, 16);

  if (ptr)
    memset(ptr, 0, total);

  return ptr;
}

void *Heap_realloc(void *ptr, size_t size)
{
  if (!ptr)
    return Heap_malloc(size);

  if (size == 0)
  {
    _aligned_free(ptr);
    return NULL;
  }

  return _aligned_realloc(ptr, size, 16);
}

void Heap_free(void *ptr)
{
  _aligned_free(ptr);
}
