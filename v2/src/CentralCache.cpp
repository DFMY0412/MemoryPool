#include "../include/CentralCache.h"
#include "../include/PageCache.h"
#include <cassert>
#include <thread>
#include <chrono>

namespace my_memoryPool_v2 {

// 延迟回收时间间隔常量定义（1秒）
const std::chrono::milliseconds CentralCache::DELAY_INTERVAL{1000};

// 每次从 PageCache 分配的页数（8页 = 32KB）
static const size_t SPAN_PAGES = 8;

/* 构造函数
   功能：初始化 CentralCache 的各个成员变量
   说明：使用 relaxed 内存序初始化原子变量，因为此时还没有并发访问 */
CentralCache::CentralCache()
{
    // 初始化所有自由链表为空指针
    for (auto& ptr : centralFreeList_)
    {
        ptr.store(nullptr, std::memory_order_relaxed);
    }

    // 初始化所有自旋锁为"未锁定"状态
    for (auto& lock : locks_)
    {
        lock.clear();
    }

    // 初始化所有延迟计数为 0
    for (auto& count : delayCounts_)
    {
        count.store(0, std::memory_order_relaxed);
    }

    // 初始化所有时间点为当前时间
    for (auto& time : lastReturnTimes_)
    {
        time = std::chrono::steady_clock::now();
    }

    // 初始化 Span 计数器为 0
    spanCount_.store(0, std::memory_order_relaxed);
}

/* 参数：index – 自由链表索引
   功能：从中央缓存获取一个内存块
   返回值：分配的内存块地址，失败返回 nullptr
   流程：
     1. 获取对应大小类的锁（自旋锁）
     2. 尝试从自由链表获取块
     3. 如果为空，从 PageCache 分配新块
     4. 释放锁并返回 */
void* CentralCache::fetchRange(size_t index)
{
    // 索引有效性检查
    if (index >= FREE_LIST_SIZE)
        return nullptr;

    // 获取对应大小类的自旋锁（acquire 内存序确保获取锁前的操作对当前线程可见）
    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield();  // 锁被占用时让出 CPU 时间片
    }

    void* result = nullptr;
    try
    {
        // 尝试从中央自由链表获取内存块
        result = centralFreeList_[index].load(std::memory_order_relaxed);

        if (!result)
        {
            // 链表为空，需要从 PageCache 分配新块
            // 计算该大小类对应的实际字节数
            size_t size = (index + 1) * ALIGNMENT;

            // 从 PageCache 获取内存
            result = fetchFromPageCache(size);

            if (!result)
            {
                // 分配失败，释放锁并返回
                locks_[index].clear(std::memory_order_release);
                return nullptr;
            }

            char* start = static_cast<char*>(result);

            // 计算需要的页数（至少 SPAN_PAGES 页）
            size_t numPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE) ?
                             SPAN_PAGES : (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

            // 计算该 Span 能切分出的块数
            size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;

            // 如果能切分出多个块，建立链表结构
            if (blockNum > 1)
            {
                // 将连续的内存块链接成链表
                for (size_t i = 1; i < blockNum; ++i)
                {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                }
                // 最后一个块的 next 置为空
                *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr;

                // 取出第一个块返回给调用者
                void* next = *reinterpret_cast<void**>(result);
                *reinterpret_cast<void**>(result) = nullptr;

                // 将剩余块放入中央自由链表
                centralFreeList_[index].store(next, std::memory_order_release);

                // 创建 Span 追踪器，记录这个 Span 的信息
                size_t trackerIndex = spanCount_++;
                if (trackerIndex < spanTrackers_.size())
                {
                    spanTrackers_[trackerIndex].spanAddr.store(start, std::memory_order_release);
                    spanTrackers_[trackerIndex].numPages.store(numPages, std::memory_order_release);
                    spanTrackers_[trackerIndex].blockCount.store(blockNum, std::memory_order_release);
                    spanTrackers_[trackerIndex].freeCount.store(blockNum - 1, std::memory_order_release);
                }
            }
        }
        else
        {
            // 链表不为空，直接取出头节点
            void* next = *reinterpret_cast<void**>(result);
            *reinterpret_cast<void**>(result) = nullptr;

            // 更新中央自由链表
            centralFreeList_[index].store(next, std::memory_order_release);

            // 更新对应 Span 的空闲块计数
            SpanTracker* tracker = getSpanTracker(result);
            if (tracker)
            {
                tracker->freeCount.fetch_sub(1, std::memory_order_release);
            }
        }
    }
    catch (...)
    {
        // 发生异常时确保锁被释放
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    // 释放锁（release 内存序确保释放锁前的操作对其他线程可见）
    locks_[index].clear(std::memory_order_release);
    return result;
}

/* 参数：start – 待归还的内存块起始地址
   参数：size – 总大小（字节数）
   参数：index – 自由链表索引
   功能：将内存块归还到中央缓存
   返回值：无
   说明：实现延迟回收机制，避免频繁的跨层内存移动 */
void CentralCache::returnRange(void* start, size_t size, size_t index)
{
    if (!start || index >= FREE_LIST_SIZE)
        return;

    // 计算块大小和块数量
    size_t blockSize = (index + 1) * ALIGNMENT;
    size_t blockCount = size / blockSize;

    // 获取锁
    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    try
    {
        // 找到链表的末尾
        void* end = start;
        size_t count = 1;
        while (*reinterpret_cast<void**>(end) != nullptr && count < blockCount) {
            end = *reinterpret_cast<void**>(end);
            count++;
        }

        // 将归还的链表连接到中央自由链表头部
        void* current = centralFreeList_[index].load(std::memory_order_relaxed);
        *reinterpret_cast<void**>(end) = current;
        centralFreeList_[index].store(start, std::memory_order_release);

        // 更新延迟计数
        size_t currentCount = delayCounts_[index].fetch_add(1, std::memory_order_relaxed) + 1;
        auto currentTime = std::chrono::steady_clock::now();

        // 检查是否应该执行延迟回收
        if (shouldPerformDelayedReturn(index, currentCount, currentTime))
        {
            performDelayedReturn(index);
        }
    }
    catch (...)
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    // 释放锁
    locks_[index].clear(std::memory_order_release);
}

/* 参数：index – 自由链表索引
   参数：currentCount – 当前延迟计数
   参数：currentTime – 当前时间点
   功能：判断是否应该执行延迟回收
   返回值：true 表示应该执行，false 表示继续延迟
   说明：满足以下任一条件时触发回收：
         1. 延迟计数达到阈值（MAX_DELAY_COUNT = 48）
         2. 距离上次回收超过时间间隔（DELAY_INTERVAL = 1秒） */
bool CentralCache::shouldPerformDelayedReturn(size_t index, size_t currentCount,
    std::chrono::steady_clock::time_point currentTime)
{
    // 延迟计数达到阈值，立即触发回收
    if (currentCount >= MAX_DELAY_COUNT)
    {
        return true;
    }

    // 检查是否超过时间间隔
    auto lastTime = lastReturnTimes_[index];
    return (currentTime - lastTime) >= DELAY_INTERVAL;
}

/* 参数：index – 自由链表索引
   功能：执行延迟回收，将完全空闲的 Span 归还给 PageCache
   返回值：无
   说明：遍历自由链表，统计每个 Span 的空闲块数，
         如果某个 Span 的所有块都空闲，则归还给 PageCache */
void CentralCache::performDelayedReturn(size_t index)
{
    // 重置延迟计数和更新时间
    delayCounts_[index].store(0, std::memory_order_relaxed);
    lastReturnTimes_[index] = std::chrono::steady_clock::now();

    // 统计每个 Span 的空闲块数量
    std::unordered_map<SpanTracker*, size_t> spanFreeCounts;
    void* currentBlock = centralFreeList_[index].load(std::memory_order_relaxed);

    while (currentBlock)
    {
        // 查找该块所属的 Span
        SpanTracker* tracker = getSpanTracker(currentBlock);
        if (tracker)
        {
            spanFreeCounts[tracker]++;
        }
        currentBlock = *reinterpret_cast<void**>(currentBlock);
    }

    // 更新每个 Span 的空闲计数，触发可能的回收
    for (const auto& [tracker, newFreeBlocks] : spanFreeCounts)
    {
        updateSpanFreeCount(tracker, newFreeBlocks, index);
    }
}

/* 参数：tracker – SpanTracker 指针
   参数：newFreeBlocks – 新增的空闲块数
   参数：index – 自由链表索引
   功能：更新 Span 的空闲块计数，如果所有块都空闲则归还给 PageCache
   返回值：无 */
void CentralCache::updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index)
{
    // 更新空闲块计数
    size_t oldFreeCount = tracker->freeCount.load(std::memory_order_relaxed);
    size_t newFreeCount = oldFreeCount + newFreeBlocks;
    tracker->freeCount.store(newFreeCount, std::memory_order_release);

    // 检查是否所有块都空闲（可以回收整个 Span）
    if (newFreeCount == tracker->blockCount.load(std::memory_order_relaxed))
    {
        // 获取 Span 的地址和页数
        void* spanAddr = tracker->spanAddr.load(std::memory_order_relaxed);
        size_t numPages = tracker->numPages.load(std::memory_order_relaxed);

        // 从中央自由链表中移除属于该 Span 的所有块
        void* head = centralFreeList_[index].load(std::memory_order_relaxed);
        void* newHead = nullptr;
        void* prev = nullptr;
        void* current = head;

        while (current)
        {
            void* next = *reinterpret_cast<void**>(current);

            // 检查当前块是否属于该 Span
            if (current >= spanAddr &&
                current < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE)
            {
                // 属于该 Span，从链表中移除
                if (prev)
                {
                    *reinterpret_cast<void**>(prev) = next;
                }
                else
                {
                    newHead = next;
                }
            }
            else
            {
                // 不属于该 Span，保留
                prev = current;
            }
            current = next;
        }

        // 更新中央自由链表
        centralFreeList_[index].store(newHead, std::memory_order_release);

        // 将 Span 归还给 PageCache
        PageCache::getInstance().deallocateSpan(spanAddr, numPages);
    }
}

/* 参数：size – 请求的字节数
   功能：从 PageCache 获取内存页
   返回值：分配的内存页起始地址
   说明：根据请求大小决定分配策略：
         1. 小于等于 SPAN_PAGES * PAGE_SIZE 时，固定分配 SPAN_PAGES 页
         2. 大于时，按需分配 */
void* CentralCache::fetchFromPageCache(size_t size)
{
    // 计算需要的页数
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE)
    {
        // 小块内存固定分配 SPAN_PAGES 页（32KB）
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    }
    else
    {
        // 大块内存按需分配
        return PageCache::getInstance().allocateSpan(numPages);
    }
}

/* 参数：blockAddr – 内存块地址
   功能：根据内存块地址查找对应的 SpanTracker
   返回值：找到返回指针，未找到返回 nullptr
   说明：遍历所有 SpanTracker，通过地址范围判断该块属于哪个 Span */
SpanTracker* CentralCache::getSpanTracker(void* blockAddr)
{
    for (size_t i = 0; i < spanCount_.load(std::memory_order_relaxed); ++i)
    {
        void* spanAddr = spanTrackers_[i].spanAddr.load(std::memory_order_relaxed);
        size_t numPages = spanTrackers_[i].numPages.load(std::memory_order_relaxed);

        // 检查 blockAddr 是否在该 Span 的地址范围内
        if (blockAddr >= spanAddr &&
            blockAddr < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE)
        {
            return &spanTrackers_[i];
        }
    }
    return nullptr;
}

} // namespace my_memoryPool_v2