#pragma once
#include "Common.h"
#include <map>
#include <mutex>

namespace my_memoryPool_v2 {

/* 页缓存类
   管理物理内存页，是内存池与系统内存之间的桥梁
   以 4KB 页为单位向系统申请内存，并管理空闲页的合并与拆分 */
class PageCache
{
public:
    static const size_t PAGE_SIZE = 4096;  // 系统页大小（4KB）

    /* 功能：获取 PageCache 实例（单例）
       返回值：PageCache 实例引用 */
    static PageCache& getInstance()
    {
        static PageCache instance;
        return instance;
    }

    /* 参数：numPages – 需要的页数
       功能：分配指定数量的连续内存页
       返回值：分配的内存页起始地址，失败返回 nullptr */
    void* allocateSpan(size_t numPages);

    /* 参数：ptr – 待释放的内存页起始地址
       参数：numPages – 内存页数量
       功能：释放内存页到页缓存，并尝试与相邻页合并
       返回值：无 */
    void deallocateSpan(void* ptr, size_t numPages);

private:
    // 私有构造函数，确保不能直接创建实例
    PageCache() = default;

    /* 参数：numPages – 需要的页数
       功能：调用系统接口分配内存
       返回值：分配的内存起始地址，失败返回 nullptr */
    void* systemAlloc(size_t numPages);

private:
    /* Span 结构体
       表示一段连续的内存页，用于管理空闲内存 */
    struct Span
    {
        void*  pageAddr;   // Span 的起始页地址
        size_t numPages;   // 该 Span 占用的页数
        Span*  next;       // 指向下一个 Span（用于链表管理）
    };

    // 空闲 Span 的映射表：key 为页数，value 为对应大小的 Span 链表头指针
    std::map<size_t, Span*> freeSpans_;

    // 地址到 Span 的映射表：用于快速查找任意地址所属的 Span
    std::map<void*, Span*> spanMap_;

    // 保护页缓存操作的互斥锁（全局锁）
    std::mutex mutex_;
};

} // namespace my_memoryPool_v2