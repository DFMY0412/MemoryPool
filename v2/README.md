# 内存池 v2 版本说明文档

## 项目概述

内存池 v2 是一个高并发内存管理系统，旨在提供高效的内存分配和释放机制，减少系统调用开销，提高内存管理性能。本版本采用三层缓存架构，包括线程缓存（ThreadCache）、中央缓存（CentralCache）和页缓存（PageCache），实现了线程本地存储和延迟回收等优化策略。

### 主要特点

- **三层缓存架构**：ThreadCache、CentralCache、PageCache 分层管理内存
- **线程本地存储**：每个线程拥有独立的 ThreadCache，减少线程竞争
- **延迟回收**：实现内存块的延迟回收机制，优化内存使用
- **内存对齐**：按 8 字节对齐，提高内存访问效率
- **大内存处理**：超过 256KB 的内存直接使用系统分配

## 架构设计

### 三层缓存架构

| 缓存层级 | 作用 | 线程安全性 | 内存大小范围 |
|---------|------|-----------|------------|
| ThreadCache | 线程本地缓存，快速分配/释放 | 无锁（线程本地） | 8B - 256KB |
| CentralCache | 中央缓存，协调内存分配 | 有锁（细粒度） | 8B - 256KB |
| PageCache | 页级缓存，管理物理内存页 | 有锁（全局） | 4KB 页为单位 |

### 内存分配流程

1. **ThreadCache**：首先尝试从线程本地的自由链表中分配内存
2. **CentralCache**：如果 ThreadCache 不足，从 CentralCache 获取内存块
3. **PageCache**：如果 CentralCache 不足，从 PageCache 分配内存页
4. **系统分配**：如果 PageCache 不足，使用 mmap 从系统分配内存

### 内存释放流程

1. **ThreadCache**：将内存块释放到线程本地的自由链表
2. **CentralCache**：当 ThreadCache 中的内存块数量超过阈值时，返回部分内存到 CentralCache
3. **PageCache**：当 CentralCache 中的内存块全部空闲时，将整个内存页返回给 PageCache
4. **系统释放**：当 PageCache 中的内存页长时间未使用时，可考虑释放回系统

## 核心组件

### 1. MemoryPool

MemoryPool 是对外提供的统一接口，封装了线程缓存的分配和释放操作。

**主要方法**：
- `allocate(size_t size)`：分配指定大小的内存
- `deallocate(void* ptr, size_t size)`：释放指定大小的内存

### 2. ThreadCache

ThreadCache 是线程本地的缓存，每个线程拥有独立的实例，实现无锁操作。

**主要特性**：
- 使用 `thread_local` 存储，确保线程隔离
- 维护多个自由链表，对应不同大小的内存块
- 当内存块数量超过阈值时，将部分内存返回给 CentralCache

**核心方法**：
- `allocate(size_t size)`：分配内存
- `deallocate(void* ptr, size_t size)`：释放内存
- `fetchFromCentralCache(size_t index)`：从 CentralCache 获取内存
- `returnToCentralCache(void* start, size_t size)`：返回内存到 CentralCache

### 3. CentralCache

CentralCache 是中央缓存，协调不同线程之间的内存分配，使用细粒度锁减少竞争。

**主要特性**：
- 维护多个自由链表，对应不同大小的内存块
- 使用 `std::atomic_flag` 实现自旋锁，减少线程阻塞
- 实现延迟回收机制，优化内存使用
- 跟踪内存块所属的 Span，便于内存管理

**核心方法**：
- `fetchRange(size_t index)`：获取内存块范围
- `returnRange(void* start, size_t size, size_t index)`：返回内存块范围
- `fetchFromPageCache(size_t size)`：从 PageCache 获取内存页
- `performDelayedReturn(size_t index)`：执行延迟回收

### 4. PageCache

PageCache 是页级缓存，管理物理内存页，是内存池与系统内存之间的桥梁。

**主要特性**：
- 以 4KB 页为单位管理内存
- 使用 `mmap` 从系统分配内存
- 实现内存页的合并和拆分
- 维护空闲内存页的映射，便于快速分配

**核心方法**：
- `allocateSpan(size_t numPages)`：分配指定数量的内存页
- `deallocateSpan(void* ptr, size_t numPages)`：释放内存页
- `systemAlloc(size_t numPages)`：从系统分配内存

## 数据结构说明

### 1. Common.h 数据结构

#### 1.1 BlockHeader 结构体

```cpp
struct BlockHeader
{
    size_t size;          // 内存块大小
    bool   inUse;         // 使用标志（true表示已分配，false表示空闲）
    BlockHeader* next;    // 指向下一个内存块（用于自由链表）
};
```

- **用途**：预留接口，用于管理内存块的元数据（当前版本未使用）
- **特点**：包含大小、使用状态和链表指针，支持内存块的链式管理

#### 1.2 SizeClass 类

```cpp
class SizeClass
{
public:
    static size_t roundUp(size_t bytes);   // 向上对齐到 ALIGNMENT 倍数
    static size_t getIndex(size_t bytes);  // 计算自由链表索引
};
```

- **用途**：负责内存大小的对齐计算和自由链表索引计算
- **常量**：
  - `ALIGNMENT = 8`：内存对齐字节数
  - `MAX_BYTES = 256KB`：单次分配的最大字节数
  - `FREE_LIST_SIZE = 32768`：自由链表数量

### 2. ThreadCache.h 数据结构

#### 2.1 ThreadCache 类成员变量

| 成员变量 | 类型 | 说明 |
|---------|------|------|
| `freeList_` | `std::array<void*, FREE_LIST_SIZE>` | 自由链表数组，每个索引对应一种大小类的空闲内存块链表 |
| `freeListSize_` | `std::array<size_t, FREE_LIST_SIZE>` | 自由链表的长度数组，记录每个链表中的空闲块数量 |

#### 2.2 内存布局示意

```
ThreadCache 中的自由链表结构：

freeList_[index]  →  [Block] → [Block] → [Block] → nullptr
                        ↓
                   通过强制类型转换将前 8 字节作为 next 指针
```

### 3. CentralCache.h 数据结构

#### 3.1 SpanTracker 结构体

```cpp
struct SpanTracker {
    std::atomic<void*> spanAddr{nullptr};   // Span 的起始地址
    std::atomic<size_t> numPages{0};       // 该 Span 占用的页数
    std::atomic<size_t> blockCount{0};      // 该 Span 中的内存块总数
    std::atomic<size_t> freeCount{0};      // 当前空闲的内存块数量
};
```

- **用途**：跟踪从 PageCache 分配的内存 Span 的使用情况
- **特点**：当 Span 中的所有内存块都空闲时，可将其归还给 PageCache

#### 3.2 CentralCache 类成员变量

| 成员变量 | 类型 | 说明 |
|---------|------|------|
| `centralFreeList_` | `std::array<std::atomic<void*>, FREE_LIST_SIZE>` | 中央缓存的自由链表数组 |
| `locks_` | `std::array<std::atomic_flag, FREE_LIST_SIZE>` | 保护每个自由链表的细粒度自旋锁 |
| `spanTrackers_` | `std::array<SpanTracker, 1024>` | Span 追踪器数组，记录所有分配的 Span 信息 |
| `spanCount_` | `std::atomic<size_t>` | 当前已分配的 Span 数量 |
| `delayCounts_` | `std::array<std::atomic<size_t>, FREE_LIST_SIZE>` | 每个大小类的延迟计数 |
| `lastReturnTimes_` | `std::array<time_point, FREE_LIST_SIZE>` | 每个大小类上次执行回收的时间点 |

- **常量**：`MAX_DELAY_COUNT = 48`，延迟回收的最大计数阈值

### 4. PageCache.h 数据结构

#### 4.1 Span 结构体

```cpp
struct Span
{
    void*  pageAddr;   // Span 的起始页地址
    size_t numPages;   // 该 Span 占用的页数
    Span*  next;       // 指向下一个 Span（用于链表管理）
};
```

- **用途**：表示一段连续的内存页，用于管理空闲内存
- **特点**：通过 next 指针串联成链表，支持空闲页的合并与拆分

#### 4.2 PageCache 类成员变量

| 成员变量 | 类型 | 说明 |
|---------|------|------|
| `freeSpans_` | `std::map<size_t, Span*>` | 空闲 Span 的映射表，key 为页数，value 为对应大小的 Span 链表头指针 |
| `spanMap_` | `std::map<void*, Span*>` | 地址到 Span 的映射表，用于快速查找任意地址所属的 Span |
| `mutex_` | `std::mutex` | 保护页缓存操作的互斥锁（全局锁） |

- **常量**：`PAGE_SIZE = 4096`，系统页大小（4KB）

#### 4.3 PageCache 内存管理示意

```
PageCache 中的空闲 Span 管理：

freeSpans_:
  ├── 1 页 → [Span] → [Span] → nullptr
  ├── 4 页 → [Span] → nullptr
  └── 8 页 → [Span] → nullptr

spanMap_:
  ├── addr1 → Span(pageAddr=addr1, numPages=4)
  ├── addr2 → Span(pageAddr=addr2, numPages=1)
  └── addr3 → Span(pageAddr=addr3, numPages=8)
```

## 5. 函数说明

### 5.1 MemoryPool 类函数

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `allocate(size_t size)` | size: 请求分配的字节数 | void* | 从内存池分配指定大小的内存，委托给 ThreadCache 进行实际分配 |
| `deallocate(void* ptr, size_t size)` | ptr: 待释放的内存指针, size: 原始请求的字节数 | void | 将内存归还到内存池，委托给 ThreadCache 进行实际释放 |

### 5.2 ThreadCache 类函数

#### 5.2.1 单例与初始化

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `getInstance()` | 无 | ThreadCache* | 获取 ThreadCache 实例（线程单例），使用 thread_local 确保线程隔离 |

#### 5.2.2 内存分配与释放

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `allocate(size_t size)` | size: 请求分配的字节数 | void* | 分配内存，优先从本地自由链表获取，不足时从 CentralCache 获取 |
| `deallocate(void* ptr, size_t size)` | ptr: 待释放的内存指针, size: 原始请求的字节数 | void | 释放内存到本地自由链表，必要时归还给 CentralCache |
| `fetchFromCentralCache(size_t index)` | index: 自由链表索引 | void* | 从 CentralCache 批量获取内存块 |
| `returnToCentralCache(void* start, size_t size)` | start: 待归还的内存块起始地址, size: 原始请求的字节数 | void | 将内存块归还给 CentralCache（批量归还） |

#### 5.2.3 辅助函数

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `shouldReturnToCentralCache(size_t index)` | index: 自由链表索引 | bool | 判断是否应该将内存归还给 CentralCache |

### 5.3 CentralCache 类函数

#### 5.3.1 单例与初始化

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `getInstance()` | 无 | CentralCache& | 获取 CentralCache 实例（单例） |

#### 5.3.2 内存管理

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `fetchRange(size_t index)` | index: 自由链表索引 | void* | 从中央缓存获取一个内存块 |
| `returnRange(void* start, size_t size, size_t index)` | start: 待归还的内存块起始地址, size: 总大小（字节数）, index: 自由链表索引 | void | 将内存块归还到中央缓存 |
| `fetchFromPageCache(size_t size)` | size: 请求的字节数 | void* | 从 PageCache 获取内存页 |
| `performDelayedReturn(size_t index)` | index: 自由链表索引 | void | 执行延迟回收，将完全空闲的 Span 归还给 PageCache |

#### 5.3.3 辅助函数

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `getSpanTracker(void* blockAddr)` | blockAddr: 内存块地址 | SpanTracker* | 根据内存块地址查找对应的 SpanTracker |
| `updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index)` | tracker: SpanTracker 指针, newFreeBlocks: 新增的空闲块数, index: 自由链表索引 | void | 更新 Span 的空闲块计数，必要时触发回收 |
| `shouldPerformDelayedReturn(size_t index, size_t currentCount, std::chrono::steady_clock::time_point currentTime)` | index: 自由链表索引, currentCount: 当前延迟计数, currentTime: 当前时间点 | bool | 判断是否应该执行延迟回收 |

### 5.4 PageCache 类函数

#### 5.4.1 单例与初始化

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `getInstance()` | 无 | PageCache& | 获取 PageCache 实例（单例） |

#### 5.4.2 内存管理

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `allocateSpan(size_t numPages)` | numPages: 需要的页数 | void* | 分配指定数量的连续内存页 |
| `deallocateSpan(void* ptr, size_t numPages)` | ptr: 待释放的内存页起始地址, numPages: 内存页数量 | void | 释放内存页到页缓存，并尝试与相邻页合并 |
| `systemAlloc(size_t numPages)` | numPages: 需要的页数 | void* | 调用系统接口分配内存 |

### 5.5 SizeClass 类函数

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `roundUp(size_t bytes)` | bytes: 原始请求的字节数 | size_t | 将字节数向上对齐到 ALIGNMENT 的倍数 |
| `getIndex(size_t bytes)` | bytes: 原始请求的字节数 | size_t | 计算请求大小对应的自由链表索引 |

## 5. 关键功能

### 内存大小计算

- **对齐计算**：使用 `SizeClass::roundUp` 方法将内存大小向上对齐到 8 的倍数
- **索引计算**：使用 `SizeClass::getIndex` 方法计算内存大小对应的自由链表索引

### 内存块管理

- **自由链表**：使用单链表管理空闲内存块
- **内存块头部**：通过指针操作实现内存块的链接，无需额外头部空间
- **批量操作**：从 CentralCache 获取内存时采用批量操作，减少锁竞争

### 延迟回收机制

- **延迟计数**：记录内存块返回次数
- **时间间隔**：当达到最大延迟计数或时间间隔时，执行回收
- **Span 跟踪**：跟踪内存块所属的 Span，当 Span 完全空闲时回收

### 线程安全

- **ThreadCache**：线程本地存储，无锁操作
- **CentralCache**：细粒度自旋锁，减少线程竞争
- **PageCache**：互斥锁保护，确保线程安全

## 性能测试

### 测试结果

| 测试场景 | Memory Pool | New/Delete | 提速百分比 |
|---------|------------|-----------|-----------|
| 小内存分配（50000次） | 15.244 ms | 11.886 ms | -28.25% |
| 多线程分配（4线程×25000次） | 26.080 ms | 30.281 ms | 13.87% |
| 混合大小分配（100000次） | 8.541 ms | 19.847 ms | 56.97% |

### 测试分析

- **小内存分配**：由于内存池的额外开销，性能略低于 New/Delete
- **多线程分配**：内存池的线程本地缓存优势明显，性能优于 New/Delete
- **混合大小分配**：内存池的缓存机制效果显著，性能大幅优于 New/Delete

## 使用方法

### 基本用法

```cpp
#include "MemoryPool.h"

// 分配内存
void* ptr = my_memoryPool_v2::MemoryPool::allocate(128);

// 使用内存
// ...

// 释放内存
my_memoryPool_v2::MemoryPool::deallocate(ptr, 128);
```

### 注意事项

- 释放内存时必须提供正确的大小参数
- 对于大于 256KB 的内存分配，会直接使用系统的 malloc/free
- 内存池会自动管理内存的分配和释放，无需手动干预

## 代码结构

```
MemmoryPool/v2/
├── include/
│   ├── Common.h         # 公共定义和常量
│   ├── MemoryPool.h     # 内存池主接口
│   ├── ThreadCache.h    # 线程缓存
│   ├── CentralCache.h   # 中央缓存
│   └── PageCache.h      # 页缓存
├── src/
│   ├── ThreadCache.cpp  # 线程缓存实现
│   ├── CentralCache.cpp # 中央缓存实现
│   └── PageCache.cpp    # 页缓存实现
└── tests/
    └── PerformanceTest.cpp # 性能测试
```

## 总结与展望

### v2 版本改进

- 实现了三层缓存架构，提高内存管理效率
- 引入线程本地存储，减少线程竞争
- 实现延迟回收机制，优化内存使用
- 提供统一的内存分配接口，简化使用

### 未来优化方向

- **内存使用监控**：添加内存使用统计和监控功能
- **自适应调整**：根据运行时情况自动调整缓存大小和回收策略
- **内存碎片管理**：进一步优化内存碎片问题
- **多平台支持**：扩展到更多平台，如 Windows、macOS 等
- **性能分析工具**：提供内存使用和性能分析工具

## 结论

内存池 v2 版本通过三层缓存架构和多种优化策略，在多线程和混合大小内存分配场景下表现优异，显著提高了内存管理性能。虽然在小内存分配场景下存在一定开销，但整体性能优于标准的 New/Delete 操作，特别是在高并发环境下。

内存池 v2 版本为 C++ 应用程序提供了一种高效、可靠的内存管理方案，适合对性能要求较高的场景，如服务器、游戏引擎、数据库等。