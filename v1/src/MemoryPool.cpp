#include "../include/MemoryPool.h"

namespace my_memoryPool_v1 {
MemoryPool::MemoryPool(size_t BlockSize)
    : BlockSize_(BlockSize), SlotSize_(0), firstBlock_(nullptr),
      curSlot_(nullptr), freeList_(nullptr), lastSlot_(nullptr) {}

MemoryPool::~MemoryPool() {
  // 把连续的block删除
  Slot *cur = firstBlock_.load(
      std::
          memory_order_relaxed); // 宽松内存序，仅保证原子性，不提供任何顺序保证
  while (cur) {
    Slot *next = cur->next.load(std::memory_order_relaxed);
    // 等同于 free(reinterpret_cast<void*>(firstBlock_));
    // 转化为 void 指针，因为 void 类型不需要调用析构函数，只释放空间
    operator delete(reinterpret_cast<void *>(
        cur)); // operator delete只释放内存，不调用析构函数
    cur = next;
  }
}

void MemoryPool::init(size_t size) {
  assert(size > 0);
  SlotSize_ = size;
  firstBlock_.store(nullptr, std::memory_order_relaxed);
  curSlot_.store(nullptr, std::memory_order_relaxed);
  freeList_.store(nullptr, std::memory_order_relaxed);
  lastSlot_.store(nullptr, std::memory_order_relaxed);
}

void *MemoryPool::allocate() {
  // 优先使用空闲链表中的内存槽
  Slot *slot = popFreeList();
  if (slot != nullptr)
    return slot;

  Slot *temp = nullptr;

  // 只在需要申请新块时才加锁
  {
    std::lock_guard<std::mutex> lock(mutexForBlock_);
    if (curSlot_.load(std::memory_order_relaxed) >=
        lastSlot_.load(std::memory_order_relaxed)) {
      // 当前内存块已无内存槽可用，开辟一块新的内存
      allocateNewBlock();
    }

    temp = curSlot_.load(std::memory_order_relaxed);
    // 正确的指针运算：先转为char*，移动后再转回Slot*
    // 让指针按“字节”移动，而不是按“对象大小”移动。
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
  // std::cout << "申请一块内存块，SlotSize: " << SlotSize_ << std::endl;
  //  头插法插入新的内存块
  void *newBlock = operator new(BlockSize_);

  // 设置新块的next指针
  Slot *newSlot = reinterpret_cast<Slot *>(newBlock);
  Slot *oldFirst = firstBlock_.load(std::memory_order_relaxed);
  newSlot->next.store(oldFirst, std::memory_order_relaxed);
  firstBlock_.store(newSlot, std::memory_order_relaxed);

  char *body = reinterpret_cast<char *>(newBlock) + sizeof(Slot *);
  size_t paddingSize =
      padPointer(body, SlotSize_); // 计算对齐需要填充内存的大小
  curSlot_.store(reinterpret_cast<Slot *>(body + paddingSize),
                 std::memory_order_relaxed);

  // 超过该标记位置，则说明该内存块已无内存槽可用，需向系统申请新的内存块
  lastSlot_.store(reinterpret_cast<Slot *>(reinterpret_cast<char *>(newBlock) +
                                           BlockSize_ - SlotSize_),
                  std::memory_order_relaxed);
}

// 让指针对齐到槽大小的倍数位置
size_t MemoryPool::padPointer(char *p, size_t align) {
  // align 是槽大小
  size_t rem = (reinterpret_cast<size_t>(p) % align);
  return rem == 0 ? 0 : (align - rem);
}

// 实现无锁入队操作
bool MemoryPool::pushFreeList(Slot *slot) {
  while (true) {
    // 获取当前头节点
    Slot *oldHead = freeList_.load(std::memory_order_relaxed);
    // 将新节点的 next 指向当前头节点
    slot->next.store(oldHead, std::memory_order_relaxed);

    // 尝试将新节点设置为头节点
    if (freeList_.compare_exchange_weak(oldHead, slot,
                                        std::memory_order_release,
                                        std::memory_order_relaxed)) {
      return true;
    }
    // 失败：说明另一个线程可能已经修改了 freeList_
    // CAS 失败则重试
  }
}

// 实现无锁出队操作
Slot *MemoryPool::popFreeList() {
  while (true) {
    Slot *oldHead = freeList_.load(std::memory_order_acquire);
    if (oldHead == nullptr)
      return nullptr; // 队列为空

    // 在访问 newHead 之前再次验证 oldHead 的有效性
    Slot *newHead = oldHead->next.load(std::memory_order_relaxed);

    // 尝试更新头结点
    // 原子性地尝试将 freeList_ 从 oldHead 更新为 newHead
    if (freeList_.compare_exchange_weak(oldHead, newHead,
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
      return oldHead;
    }
    // 失败：说明另一个线程可能已经修改了 freeList_
    // CAS 失败则重试
  }
}

void HashBucket::initMemoryPool() {
  for (int i = 0; i < MEMORY_POOL_NUM; i++) {
    getMemoryPool(i).init((i + 1) * SLOT_BASE_SIZE);
  }
}

// 单例模式
MemoryPool &HashBucket::getMemoryPool(int index) {
  static MemoryPool memoryPool[MEMORY_POOL_NUM];
  return memoryPool[index];
}

} // namespace my_memoryPool_v1