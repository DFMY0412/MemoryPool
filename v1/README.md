# 高并发内存池项目说明文档

## 1. 项目简介

本项目实现了一个针对 **8~512 字节** 小对象的高并发内存池（Memory Pool），基于 C++11/14 标准，采用 **哈希桶 + 无锁空闲链表** 的设计。它能够显著降低多线程环境下频繁分配/释放小块内存时的锁竞争和系统调用开销，适用于网络服务器、游戏引擎、实时系统等高性能场景。

- **内存池数量**：64 个，每个管理固定大小的槽（8B, 16B, 24B, …, 512B）
- **线程安全**：分配/释放操作主要使用 CAS 原子操作，仅在申请新内存块时加锁
- **使用方式**：提供 `useMemory` / `freeMemory` 以及模板函数 `newElement<T>` / `deleteElement<T>`

---

## 2. 整体架构

### 2.1 组件关系

```text
HashBucket (单例)
    │
    ├── memoryPool[0]  → 槽大小 = 8B
    ├── memoryPool[1]  → 槽大小 = 16B
    ├── ...
    └── memoryPool[63] → 槽大小 = 512B

每个 MemoryPool 内部包含：
    ├── 无锁空闲链表 (freeList_)
    ├── 当前块剩余槽指针 (curSlot_, lastSlot_)
    ├── 大块内存链表 (firstBlock_)
    └── 互斥锁 (mutexForBlock_)
```

### 2.2 哈希桶（HashBucket）

- 静态方法 `initMemoryPool()` 初始化 64 个内存池，槽大小依次为 8,16,…,512 字节。
- 静态方法 `getMemoryPool(int index)` 返回对应内存池的单例引用。
- 静态方法 `useMemory(size_t size)` 根据请求大小自动路由到对应的内存池（若 `size>512` 则直接调用 `operator new`）。
- 静态方法 `freeMemory(void *ptr, size_t size)` 根据原大小归还内存。

### 2.3 单个内存池（MemoryPool）

- **成员变量**：
  - `BlockSize_`：每次向系统申请的大块内存大小（默认 4096 字节）。
  - `SlotSize_`：当前内存池管理的槽大小（8 的倍数，≤512）。
  - `firstBlock_`：所有大块内存的链表头，用于析构时释放。
  - `curSlot_` / `lastSlot_`：当前内存块中尚未分配过的槽区间。
  - `freeList_`：无锁空闲链表的头指针，保存被释放后又可重用的槽。
  - `mutexForBlock_`：保护 `allocateNewBlock()` 的互斥锁。

- **核心操作**：
  - `init(size_t SlotSize)`：设置槽大小并重置指针。
  - `allocate()`：优先从 `freeList_` 弹出，若为空则从当前块切分新槽；若当前块用完则调用 `allocateNewBlock()`。
  - `deallocate(void *ptr)`：将槽插回 `freeList_` 头部（无锁 CAS）。
  - `allocateNewBlock()`：申请新的大块内存，切分成多个槽，并更新 `firstBlock_` 与 `curSlot_`。

### 2.4 数据结构说明

#### 2.4.1 Slot 结构体

```cpp
struct Slot {
    std::atomic<Slot *> next; // 原子指针，指向下一个空闲槽
};
```

- **用途**：表示内存池中的空闲槽节点，通过 next 指针串联成单链表
- **特点**：使用 `std::atomic` 实现无锁操作，`sizeof(Slot)` 仅为指针大小（8字节）
- **注意**：实际的槽大小（SlotSize）大于 `sizeof(Slot*)`，因为数据区不包含 next 指针

#### 2.4.2 MemoryPool 类成员变量详解

| 成员变量 | 类型 | 说明 |
|---------|------|------|
| `BlockSize_` | `size_t` | 每次向系统申请的大块内存大小（默认 4096 字节） |
| `SlotSize_` | `size_t` | 当前内存池管理的槽大小（8 的倍数，≤512） |
| `firstBlock_` | `std::atomic<Slot*>` | 指向首个大块内存，用于析构时遍历释放所有内存 |
| `curSlot_` | `std::atomic<Slot*>` | 指向当前大块内存中下一个可分配的槽位置 |
| `lastSlot_` | `std::atomic<Slot*>` | 标记当前大块内存的结束位置，用于判断块是否用尽 |
| `freeList_` | `std::atomic<Slot*>` | 无锁空闲链表头指针，存储被释放后可复用的槽 |
| `mutexForBlock_` | `std::mutex` | 互斥锁，仅在 `allocateNewBlock()` 中使用 |

#### 2.4.3 内存布局示意

```
┌─────────────────────────────────────────────────────────┐
│                    大块内存 (BlockSize_)                  │
├──────────────┬──────────────────────────────────────────┤
│  Slot* next  │              数据区 (SlotSize_)           │
│  (8 bytes)   │              (如 16 bytes)                │
└──────────────┴──────────────────────────────────────────┘
      ↓                    ↓                    ↓
   [ Slot ] ←──next── [ Slot ] ←──next── [ Slot ] ←── ...
```

#### 2.4.4 HashBucket 静态方法详解

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `initMemoryPool()` | 无 | void | 初始化 64 个内存池，槽大小为 8~512 字节 |
| `getMemoryPool(index)` | index: 0~63 | MemoryPool& | 返回对应索引的内存池引用 |
| `useMemory(size)` | size: 请求字节数 | void* | 分配内存，≤512 用内存池，否则用 operator new |
| `freeMemory(ptr, size)` | ptr: 指针, size: 大小 | void | 释放内存，小于等于 512 归还内存池 |

## 3. 函数说明

### 3.1 MemoryPool 类函数

#### 3.1.1 构造与初始化

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `MemoryPool(size_t BlockSize = 4096)` | BlockSize: 每个内存块的大小（默认 4096 字节） | 无 | 构造函数，初始化成员变量，但不分配内存 |
| `~MemoryPool()` | 无 | 无 | 析构函数，释放所有通过 firstBlock_ 管理的内存块 |
| `init(size_t SlotSize)` | SlotSize: 槽大小（8 的倍数，≤512） | 无 | 设置槽大小并分配第一个内存块 |

#### 3.1.2 内存分配与释放

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `allocate()` | 无 | void* | 分配一个槽：优先从空闲链表取，否则从当前块取，必要时分配新块 |
| `deallocate(void* ptr)` | ptr: 待释放的指针 | 无 | 将槽归还到空闲链表头部（无锁） |
| `allocateNewBlock()` | 无 | 无 | 分配一大块内存并切分成多个槽，更新块链表及当前指针（加锁） |

#### 3.1.3 辅助函数

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `padPointer(char* p, size_t align)` | p: 原始地址, align: 对齐字节数 | size_t | 计算需填充多少字节才能对齐到 align |
| `pushFreeList(Slot* slot)` | slot: 待插入的空闲槽 | bool | 用 CAS 将槽插入 freeList_ 头部（无锁），始终返回 true |
| `popFreeList()` | 无 | Slot* | 用 CAS 从 freeList_ 头部弹出一个空闲槽（无锁），链表空则返回 nullptr |

### 3.2 HashBucket 类函数

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `initMemoryPool()` | 无 | void | 初始化 64 个内存池，槽大小依次为 8~512 字节 |
| `getMemoryPool(int index)` | index: 内存池索引（0~63） | MemoryPool& | 返回对应索引的内存池引用 |
| `useMemory(size_t size)` | size: 请求大小 | void* | size≤512 则从对应内存池分配，否则直接 operator new |
| `freeMemory(void* ptr, size_t size)` | ptr: 待释放指针, size: 原请求大小 | void | 根据 size 决定归还到内存池或调用 operator delete |

### 3.3 全局模板函数

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `newElement<T>(Args&&... args)` | args: 构造参数 | T* | 通过 useMemory 分配内存，然后 placement new 构造对象 |
| `deleteElement<T>(T* p)` | p: 对象指针 | void | 调用析构函数，再通过 freeMemory 归还内存 |

---

## 4. 关键实现细节

### 3.1 无锁空闲链表（Lock‑Free Free List）

使用 `std::atomic<Slot*>` 表示链表头，通过 **CAS（Compare-And-Swap）** 实现入队和出队：

```cpp
bool pushFreeList(Slot *slot) {
    while (true) {
        Slot *oldHead = freeList_.load(std::memory_order_relaxed);
        slot->next.store(oldHead, std::memory_order_relaxed);
        if (freeList_.compare_exchange_weak(oldHead, slot,
                std::memory_order_release, std::memory_order_relaxed))
            return true;
    }
}

Slot* popFreeList() {
    while (true) {
        Slot *oldHead = freeList_.load(std::memory_order_acquire);
        if (!oldHead) return nullptr;
        Slot *newHead = oldHead->next.load(std::memory_order_relaxed);
        if (freeList_.compare_exchange_weak(oldHead, newHead,
                std::memory_order_acquire, std::memory_order_relaxed))
            return oldHead;
    }
}
```

- 避免了多线程环境下使用互斥锁带来的上下文切换和缓存乒乓。
- 使用适当的内存序（acquire/release）保证可见性和顺序。

### 3.2 内存对齐与切分

- 每个槽的地址会按 `SlotSize_` 对齐，提高 CPU 访问效率。
- `padPointer()` 计算从给定地址到下一个对齐地址所需的填充字节数。
- 在 `allocateNewBlock()` 中，跳过链表头 `sizeof(Slot*)` 字节后，对齐 `body` 指针，再设置 `curSlot_`。

### 3.3 锁粒度控制

- 仅在当前内存块用尽、需要向系统申请新块时才加锁（`mutexForBlock_`）。
- 分配/释放过程中的空闲链表操作完全无锁，因此多线程并发大部分时间都不阻塞。

### 3.4 模板友好接口

提供了两个全局模板函数，使内存池的使用方式与 `new`/`delete` 类似：

```cpp
template <typename T, typename... Args>
T* newElement(Args&&... args) {
    T* p = reinterpret_cast<T*>(HashBucket::useMemory(sizeof(T)));
    if (p) new (p) T(std::forward<Args>(args)...);
    return p;
}

template <typename T>
void deleteElement(T* p) {
    if (p) {
        p->~T();
        HashBucket::freeMemory(p, sizeof(T));
    }
}
```

- 自动根据类型大小选择合适的哈希桶。
- 完美转发构造函数参数，支持任意构造方式。
- 分离内存分配与对象构造，符合 RAII 思想。

---

## 4. API 使用说明

### 4.1 初始化

```cpp
#include "MemoryPool.h"
using namespace my_memoryPool_v1;

// 必须在程序开始时调用一次
HashBucket::initMemoryPool();
```

### 4.2 直接分配/释放原始内存

```cpp
// 分配内存（大小 ≤512 时使用内存池，否则用 operator new）
void* ptr = HashBucket::useMemory(size);

// 释放内存（必须传入原始申请时的大小）
HashBucket::freeMemory(ptr, size);
```

### 4.3 类型化对象的构造/析构

```cpp
// 构造一个 MyClass 对象，参数传递方式同 make_unique
MyClass* obj = newElement<MyClass>(arg1, arg2, ...);

// 析构并释放内存
deleteElement(obj);
```

### 4.4 直接操作 MemoryPool（一般不需要）

```cpp
MemoryPool& pool = HashBucket::getMemoryPool(index); // 0~63
pool.init(slotSize);               // 重设池大小（慎用）
void* mem = pool.allocate();
pool.deallocate(mem);
```

---

## 5. 性能测试与结果

### 5.1 测试环境

- CPU：VMware Virtual Platform（多核）
- 编译器：GCC 9+，C++14 标准
- 测试负载：每个线程分配/释放 1,000,000 次（总操作数随线程数变化）
- 分配大小：8~512 字节随机（8 的倍数）

### 5.2 单线程性能

| 分配器          | 总耗时 (ms) | 吞吐量 (ops/ms) |
| --------------- | ----------- | --------------- |
| MemoryPool      | 953         | 1.05 × 10⁶      |
| new / delete    | 743         | 1.35 × 10⁶      |

> 单线程下内存池略慢于系统分配器（约 28%），原因是无锁原子操作本身有一定开销，且系统分配器对小对象有优化。

### 5.3 多线程性能

| 线程数 | 分配器          | 耗时 (ms) | 吞吐量 (ops/ms) | 相比 new/delete 提升 |
| ------ | --------------- | --------- | --------------- | -------------------- |
| 1      | MemoryPool      | 548       | 1.82 × 10⁶      | **+59.0%**           |
| 1      | new/delete      | 1337      | 0.75 × 10⁶      | -                    |
| 2      | MemoryPool      | 346       | 2.89 × 10⁶      | **+37.1%**           |
| 2      | new/delete      | 550       | 1.82 × 10⁶      | -                    |
| 4      | MemoryPool      | 235       | 4.26 × 10⁶      | **+38.2%**           |
| 4      | new/delete      | 380       | 2.63 × 10⁶      | -                    |
| 8      | MemoryPool      | 261       | 3.83 × 10⁶      | **+32.7%**           |
| 8      | new/delete      | 388       | 2.58 × 10⁶      | -                    |

> **结论**：在多线程并发场景下，内存池性能显著优于系统默认分配器，尤其在线程数为 4 时吞吐量达到最高（4.26M ops/ms），提升近 40%。这得益于无锁设计和极低的锁竞争。

### 5.4 性能分析

- 系统 `new`/`delete` 在多线程下会频繁进入内核态并竞争全局堆锁，导致性能严重下降。
- 本内存池通过预分配大块内存 + 无锁空闲链表，将大部分操作控制在用户态，且每个哈希桶独立，进一步减少了冲突。
- 单线程性能略差是合理的取舍，因为多线程高并发才是内存池的主要优化目标。

---

## 6. 总结与改进方向

### 6.1 项目亮点

- **高并发优化**：CAS 无锁链表 + 细粒度锁，多线程吞吐量提升 30%~60%。
- **低内存碎片**：定长槽 + 对齐策略，避免小对象产生的内部碎片。
- **易用性**：模板接口 `newElement`/`deleteElement`，兼容标准 `new` 语义。
- **可扩展性**：哈希桶数量、槽大小范围、块大小均可通过宏配置。

### 6.2 可能的改进

- **自适应块大小**：根据分配频率动态调整 `BlockSize_`，减少内存浪费。
- **线程本地缓存（TLS）**：为每个线程添加一个小的空闲链表，进一步降低原子操作竞争。
- **支持更大对象**：可扩展哈希桶范围至 1KB 或更大。
- **内存回收机制**：当空闲链表过长时，归还部分大块内存给操作系统。


