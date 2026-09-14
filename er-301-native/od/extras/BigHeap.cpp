#include <od/extras/BigHeap.h>
#include <hal/log.h>
#include <hal/heap.h>

namespace od
{

  BigHeap::BigHeap()
  {
    uintptr_t heapStart = Heap_getUnusedMemoryStart();
    uint32_t heapSize = Heap_getUnusedMemorySize();
    mAllocator.allocateFrom((char *)heapStart, heapSize);
  }

  void BigHeap::init()
  {
    uintptr_t heapStart = Heap_getUnusedMemoryStart();
    uint32_t heapSize = Heap_getUnusedMemorySize();
    singleton().mAllocator.allocateFrom((char *)heapStart, heapSize);
  }

  void BigHeap::shutdown()
  {
    BigHeap &heap = singleton();
    if (heap.mAllocator.remaining() != heap.mAllocator.size())
    {
      logWarn("BigHeap shutdown with %d bytes still allocated.",
              heap.mAllocator.used());
    }
    heap.mAllocator.allocateFrom(0, 0);
  }

  BigHeap &BigHeap::singleton()
  {
    static BigHeap bigHeap;
    return bigHeap;
  }

  char *BigHeap::allocate(int bytes)
  {
    return singleton().mAllocator.allocate(bytes);
  }

  char *BigHeap::allocateZeroed(int bytes)
  {
    return singleton().mAllocator.allocateZeroed(bytes);
  }

  void BigHeap::free(char *ptr)
  {
    return singleton().mAllocator.free(ptr);
  }

  int BigHeap::size(int unit)
  {
    return singleton().mAllocator.size() / unit;
  }

  int BigHeap::remaining(int unit)
  {
    return singleton().mAllocator.remaining() / unit;
  }

  int BigHeap::largest(int unit)
  {
    return singleton().mAllocator.largest() / unit;
  }

  void BigHeap::print()
  {
    logInfo("BigHeap: size=%dMB free=%dMB", size(1024 * 1024), remaining(1024 * 1024));
    singleton().mAllocator.printSections();
  }

} // namespace od