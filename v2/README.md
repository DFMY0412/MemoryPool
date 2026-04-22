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

## 关键功能

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