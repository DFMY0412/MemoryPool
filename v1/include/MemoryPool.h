// MemoryPool.h
#pragma once

#include <atomic>
#include <cassert>
#include <mutex>

namespace my_memoryPool_v1 {
#define MEMORY_POOL_NUM 64 // 哈希桶中内存池的数量
#define SLOT_BASE_SIZE 8   // 槽的基础大小(8字节对齐)
#define MAX_SLOT_SIZE 512  // 最大槽大小(512字节)

/* 具体内存池的槽大小没法确定，因为每个内存池的槽大小不同(8的倍数)
   所以这个槽结构体的sizeof 不是实际的槽大小 */
struct Slot {
  std::atomic<Slot *> next; // 原子指针
};

class MemoryPool {
public:
  /*参数：BlockSize – 每个内存块的大小（默认 4096 字节）
  功能：构造函数，初始化成员变量，但不分配内存
  返回值：无*/
  MemoryPool(size_t BlockSize = 4096);

  /*参数：无
  功能：析构函数，释放所有通过 firstBlock_ 管理的内存块
  返回值：无*/
  ~MemoryPool();

  /*参数：SlotSize – 槽大小（8 的倍数，≤512）
  功能：设置槽大小并分配第一个内存块
  返回值：无*/
  void init(size_t);

  /*参数：无
  功能：分配一个槽：优先从空闲链表取，否则从当前块取，必要时分配新块
  返回值：分配的内存地址，失败返回 nullptr*/
  void *allocate();

  /*参数：ptr – 待释放的指针
  功能：将槽归还到空闲链表头部（无锁）
  返回值：无*/
  void deallocate(void *ptr);

private:
  /*参数：无
  功能：分配一大块内存并切分成多个槽，更新块链表及当前指针（加锁）
  返回值：无*/
  void allocateNewBlock();

  /*参数：p – 原始地址，align – 对齐字节数
  功能：计算需填充多少字节才能对齐到 align
  返回值：填充字节数*/
  size_t padPointer(char *p, size_t align);

  /*参数：slot – 待插入的空闲槽
  功能：用 CAS 将槽插入 freeList_ 头部（无锁）
  返回值：始终返回 true*/
  bool pushFreeList(Slot *slot);

  /*参数：无
  功能：用 CAS 从 freeList_ 头部弹出一个空闲槽（无锁）
  返回值：弹出的槽指针，链表空则返回 nullptr*/
  Slot *popFreeList();

private:
  // 每个大块内存的大小（默认 4096 字节）
  size_t BlockSize_;

  // 当前内存池管理的每个槽的大小（8 的倍数，不超过 512）
  size_t SlotSize_;

  // 指向首个内存块，用于遍历所有块（便于析构时释放）。
  std::atomic<Slot *> firstBlock_;

  // 指向当前内存块中尚未分配过的槽。
  std::atomic<Slot *> curSlot_;

  // 指向空闲的槽(被使用过后又被释放的槽)
  std::atomic<Slot *> freeList_;

  // 标记当前内存块的末尾，当 curSlot_ 到达该位置时需分配新块。
  std::atomic<Slot *> lastSlot_;

  // 互斥锁，保护分配新内存块的过程，防止多线程重复创建。
  std::mutex mutexForBlock_;
};

// 哈希桶，每个桶管理一个内存池
class HashBucket {
public:
  /*参数：无
  功能：初始化 64 个内存池，槽大小依次为 8~512 字节
  返回值：无*/
  static void initMemoryPool();

  /*参数：index – 内存池索引（0~63）
  功能：返回对应索引的内存池引用
  返回值：内存池引用*/
  static MemoryPool &getMemoryPool(int index);

  // 在类定义内定义的成员函数是隐式内联的（编译器会将其视为内联请求）

  /*参数：size – 请求大小
  功能：size≤512 则从对应内存池分配，否则直接 operator new
  返回值：分配的内存地址*/
  static void *useMemory(size_t size) {
    if (size <= 0)
      return nullptr;
    if (size > MAX_SLOT_SIZE) // 大于512字节的内存，则使用new
      return operator new(size);
    // 相当于size / 8 向上取整（因为分配内存只能大不能小）
    return getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).allocate();
  }

  /*参数：ptr – 待释放指针，size – 原请求大小
  功能：根据 size 决定归还到内存池或调用 operator delete
  返回值：无*/
  static void freeMemory(void *ptr, size_t size) {
    if (!ptr)
      return;
    if (size > MAX_SLOT_SIZE) {
      operator delete(ptr);
      return;
    }
    getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).deallocate(ptr);
  }

  /*参数：args – 构造参数
  功能：通过 useMemory 分配内存，然后 placement new 构造对象
  返回值：对象指针，失败返回 nullptr*/
  template <typename T, typename... Args> friend T *newElement(Args &&...args);

  /*参数：p – 对象指针
  功能：调用析构函数，再通过 freeMemory 归还内存
  返回值：无*/
  template <typename T> friend void deleteElement(T *p);
};

template <typename T, typename... Args>
T *newElement(Args &&...args) {
  T *p = nullptr;
  // 根据元素大小选取合适的内存池分配内存
  if ((p = reinterpret_cast<T *>(HashBucket::useMemory(sizeof(T)))) != nullptr)
    // placement new特性,在已分配的内存上调用构造函数
    new (p) T(std::forward<Args>(args)...); // 完美转发

  return p;
}

template <typename T>
void deleteElement(T *p) {
  // 对象析构
  if (p) {
    p->~T();
    // 内存回收
    HashBucket::freeMemory(reinterpret_cast<void *>(p), sizeof(T));
  }
}

/*编译模型要求：模板不是真正的代码，而是一个蓝图。编译器在遇到模板使用时（即实例化）需要看到完整的模板定义，才能生成具体的函数代码。如果定义放在源文件（.cpp）中，其他包含该模板声明的文件将看不到定义，导致链接错误。
实例化时机：模板的实例化发生在编译阶段，发生在每个使用该模板的翻译单元中。为了让所有需要实例化的地方都能访问到定义，必须将定义放在头文件中。*/
} // namespace my_memoryPool_v1

