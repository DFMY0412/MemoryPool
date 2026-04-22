#pragma once
#include "Common.h"
#include <mutex>
#include <unordered_map>
#include <array>
#include <atomic>
#include <chrono>

namespace my_memoryPool_v2 {

/* Span 追踪器结构体
   用于跟踪从 PageCache 分配的内存 Span 的使用情况
   当 Span 中的所有内存块都空闲时，可将其归还给 PageCache */
struct SpanTracker {
    std::atomic<void*> spanAddr{nullptr};   // Span 的起始地址
    std::atomic<size_t> numPages{0};       // 该 Span 占用的页数
    std::atomic<size_t> blockCount{0};      // 该 Span 中的内存块总数
    std::atomic<size_t> freeCount{0};      // 当前空闲的内存块数量
};

/* 中央缓存类
   协调不同线程之间的内存分配，使用细粒度锁减少竞争
   维护多个自由链表，每个大小类对应一个链表 */
class CentralCache
{
public:
    /* 功能：获取 CentralCache 实例（单例）
       返回值：CentralCache 实例引用 */
    static CentralCache& getInstance()
    {
        static CentralCache instance;
        return instance;
    }

    /* 参数：index – 自由链表索引
       功能：从中央缓存获取一个内存块
       返回值：分配的内存块地址，失败返回 nullptr */
    void* fetchRange(size_t index);

    /* 参数：start – 待归还的内存块起始地址
       参数：size – 总大小（字节数）
       参数：index – 自由链表索引
       功能：将内存块归还到中央缓存
       返回值：无 */
    void returnRange(void* start, size_t size, size_t index);

private:
    // 私有构造函数，确保不能直接创建实例
    CentralCache();

    /* 参数：size – 请求的字节数
       功能：从 PageCache 获取内存页
       返回值：分配的内存页起始地址 */
    void* fetchFromPageCache(size_t size);

    /* 参数：blockAddr – 内存块地址
       功能：根据内存块地址查找对应的 SpanTracker
       返回值：找到返回指针，未找到返回 nullptr */
    SpanTracker* getSpanTracker(void* blockAddr);

    /* 参数：tracker – SpanTracker 指针
       参数：newFreeBlocks – 新增的空闲块数
       参数：index – 自由链表索引
       功能：更新 Span 的空闲块计数，必要时触发回收
       返回值：无 */
    void updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index);

    /* 参数：index – 自由链表索引
       参数：currentCount – 当前延迟计数
       参数：currentTime – 当前时间点
       功能：判断是否应该执行延迟回收
       返回值：true 表示应该执行，false 表示继续延迟 */
    bool shouldPerformDelayedReturn(size_t index, size_t currentCount, std::chrono::steady_clock::time_point currentTime);

    /* 参数：index – 自由链表索引
       功能：执行延迟回收，将完全空闲的 Span 归还给 PageCache
       返回值：无 */
    void performDelayedReturn(size_t index);

private:
    // 中央缓存的自由链表数组（原子指针）
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;

    // 保护每个自由链表的细粒度自旋锁（每个大小类独立锁）
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;

    // Span 追踪器数组，记录所有分配的 Span 信息
    std::array<SpanTracker, 1024> spanTrackers_;

    // 当前已分配的 Span 数量（原子操作）
    std::atomic<size_t> spanCount_{0};

    // 延迟回收的最大计数阈值（超过此值立即触发回收）
    static const size_t MAX_DELAY_COUNT = 48;

    // 每个大小类的延迟计数（用于延迟回收机制）
    std::array<std::atomic<size_t>, FREE_LIST_SIZE> delayCounts_;

    // 每个大小类上次执行回收的时间点
    std::array<std::chrono::steady_clock::time_point, FREE_LIST_SIZE> lastReturnTimes_;

    // 延迟回收的时间间隔阈值（1秒）
    static const std::chrono::milliseconds DELAY_INTERVAL;
};

} // namespace my_memoryPool_v2