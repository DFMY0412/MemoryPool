#include "MemoryPool.h"
#include <cassert>
#include <cstring> // for memset
#include <stdexcept>
namespace my_memoryPool {

MemoryPool::MemoryPool(size_t BlockSize)
    : BlockSize_(BlockSize), SlotSize_(0) {
    firstBlock_.store(nullptr, std::memory_order_relaxed);
    curSlot_.store(nullptr, std::memory_order_relaxed);
    freeList_.store(nullptr, std::memory_order_relaxed);
    lastSlot_.store(nullptr, std::memory_order_relaxed);
}

MemoryPool::~MemoryPool() {
    Slot *cur = firstBlock_.load(std::memory_order_relaxed);
    while (cur) {
        Slot *next = cur->next.load(std::memory_order_relaxed);
        operator delete(reinterpret_cast<void *>(cur));
        cur = next;
    }
}

void MemoryPool::init(size_t size) {
    assert(size > 0);
    // 确保槽大小是8的倍数且不超过最大值
    SlotSize_ = (size + 7) & (~7);
    if (SlotSize_ > MAX_SLOT_SIZE) {
        SlotSize_ = MAX_SLOT_SIZE;
    }
    firstBlock_.store(nullptr, std::memory_order_relaxed);
    curSlot_.store(nullptr, std::memory_order_relaxed);
    freeList_.store(nullptr, std::memory_order_relaxed);
    lastSlot_.store(nullptr, std::memory_order_relaxed);

    // 初始化后分配第一个内存块
    allocateNewBlock();
}

void *MemoryPool::allocate() {
    Slot *slot = popFreeList();
    if (slot != nullptr)
        return slot;

    Slot *temp = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutexForBlock_);
        if (curSlot_.load(std::memory_order_relaxed) >=
            lastSlot_.load(std::memory_order_relaxed)) {
            allocateNewBlock();
        }

        temp = curSlot_.load(std::memory_order_relaxed);
        Slot *nextSlot =
            reinterpret_cast<Slot *>(reinterpret_cast<char *>(temp) + SlotSize_);
        curSlot_.store(nextSlot, std::memory_order_relaxed);
    }

    return temp;
}

void MemoryPool::deallocate(void *ptr) {
    if (!ptr)
        return;

    Slot *slot = reinterpret_cast<Slot *>(ptr);
    pushFreeList(slot);
}

void MemoryPool::allocateNewBlock() {
    void *newBlock = operator new(BlockSize_);
    Slot *newSlot = reinterpret_cast<Slot *>(newBlock);
    Slot *oldFirst = firstBlock_.load(std::memory_order_relaxed);
    newSlot->next.store(oldFirst, std::memory_order_relaxed);
    firstBlock_.store(newSlot, std::memory_order_relaxed);

    char *body = reinterpret_cast<char *>(newBlock) + sizeof(Slot *);
    size_t paddingSize = padPointer(body, SlotSize_);
    curSlot_.store(reinterpret_cast<Slot *>(body + paddingSize),
                   std::memory_order_relaxed);

    lastSlot_.store(reinterpret_cast<Slot *>(reinterpret_cast<char *>(newBlock) +
                                             BlockSize_ - SlotSize_),
                    std::memory_order_relaxed);
}

size_t MemoryPool::padPointer(char *p, size_t align) {
    size_t rem = (reinterpret_cast<size_t>(p) % align);
    return rem == 0 ? 0 : (align - rem);
}

bool MemoryPool::pushFreeList(Slot *slot) {
    while (true) {
        Slot *oldHead = freeList_.load(std::memory_order_relaxed);
        slot->next.store(oldHead, std::memory_order_relaxed);
        if (freeList_.compare_exchange_weak(oldHead, slot,
                                            std::memory_order_release,
                                            std::memory_order_relaxed)) {
            return true;
        }
    }
}

Slot *MemoryPool::popFreeList() {
    while (true) {
        Slot *oldHead = freeList_.load(std::memory_order_acquire);
        if (oldHead == nullptr)
            return nullptr;

        Slot *newHead = oldHead->next.load(std::memory_order_relaxed);
        if (freeList_.compare_exchange_weak(oldHead, newHead,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
            return oldHead;
        }
    }
}

void HashBucket::initMemoryPool() {
    static bool initialized = false;
    if (initialized) return;

    for (int i = 0; i < MEMORY_POOL_NUM; i++) {
        size_t slotSize = (i + 1) * SLOT_BASE_SIZE;
        if (slotSize <= MAX_SLOT_SIZE) {
            getMemoryPool(i).init(slotSize);
        }
    }
    initialized = true;
}

MemoryPool &HashBucket::getMemoryPool(int index) {
    static MemoryPool memoryPools[MEMORY_POOL_NUM];
    if (index < 0 || index >= MEMORY_POOL_NUM) {
        throw std::out_of_range("Index out of range in HashBucket::getMemoryPool");
    }
    return memoryPools[index];
}

void *HashBucket::useMemory(size_t size) {
    if (size <= 0)
        return nullptr;
    if (size > MAX_SLOT_SIZE)
        return operator new(size);
    return getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).allocate();
}

void HashBucket::freeMemory(void *ptr, size_t size) {
    if (!ptr)
        return;
    if (size > MAX_SLOT_SIZE) {
        operator delete(ptr);
        return;
    }
    getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).deallocate(ptr);
}

} // namespace my_memoryPool