#pragma once

#include <atomic>
#include <mutex>
#include <cstddef> // for size_t
// #include <new>     // for placement new and std::hardware_destructive_interference_size

namespace my_memoryPool {

#ifndef __cpp_lib_hardware_destructive_interference_size
constexpr size_t CACHE_LINE_SIZE = 64;
#else
constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#endif

#define MEMORY_POOL_NUM 64
#define SLOT_BASE_SIZE 8
#define MAX_SLOT_SIZE 512

struct Slot {
    std::atomic<Slot *> next;
};

class alignas(CACHE_LINE_SIZE) MemoryPool {
public:
    explicit MemoryPool(size_t BlockSize = 4096);

    ~MemoryPool();

    void init(size_t size);

    void *allocate();

    void deallocate(void *ptr);

private:
    void allocateNewBlock();

    size_t padPointer(char *p, size_t align);

    bool pushFreeList(Slot *slot);

    Slot *popFreeList();

private:
    size_t BlockSize_;
    size_t SlotSize_;
    std::atomic<Slot *> firstBlock_;
    std::atomic<Slot *> curSlot_;
    std::atomic<Slot *> freeList_;
    std::atomic<Slot *> lastSlot_;
    std::mutex mutexForBlock_; // 对于非频繁访问的块分配，锁开销可接受
};

class HashBucket {
public:
    static void initMemoryPool();

    static MemoryPool &getMemoryPool(int index);

    static void *useMemory(size_t size);

    static void freeMemory(void *ptr, size_t size);

    template <typename T, typename... Args>
    static T *newElement(Args &&...args);

    template <typename T>
    static void deleteElement(T *p);
};

template <typename T, typename... Args>
T *HashBucket::newElement(Args &&...args) {
    T *p = nullptr;
    if ((p = reinterpret_cast<T *>(useMemory(sizeof(T)))) != nullptr)
        new (p) T(std::forward<Args>(args)...);

    return p;
}

template <typename T>
void HashBucket::deleteElement(T *p) {
    if (p) {
        p->~T();
        freeMemory(reinterpret_cast<void *>(p), sizeof(T));
    }
}

} // namespace my_memoryPool