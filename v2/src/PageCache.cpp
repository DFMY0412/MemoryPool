#include "PageCache.h"
#include <sys/mman.h>
#include <cstring>

namespace my_memoryPool_v2 {

/* 参数：numPages – 需要的页数
   功能：分配指定数量的连续内存页
   返回值：分配的内存页起始地址，失败返回 nullptr
   流程：
     1. 在空闲 Span 映射表中查找合适的空闲 Span
     2. 如果找到，分配并可能拆分剩余部分
     3. 如果没找到，从系统分配新的内存
   说明：使用最佳适配算法，寻找最小的能够满足需求的空闲 Span */
void* PageCache::allocateSpan(size_t numPages)
{
    // 获取互斥锁保护
    std::lock_guard<std::mutex> lock(mutex_);

    // 在空闲 Span 映射表中查找 >= numPages 的最小 Span
    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end())
    {
        // 找到合适的 Span
        Span* span = it->second;

        // 从链表中移除该 Span
        if (span->next)
        {
            // 更新链表头
            freeSpans_[it->first] = span->next;
        }
        else
        {
            // 链表中只有这一个 Span，删除该键值对
            freeSpans_.erase(it);
        }

        // 如果 Span 过大，拆分成两个 Span
        if (span->numPages > numPages)
        {
            // 创建新的 Span 保存剩余的页
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) +
                                numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            // 将新 Span 添加到空闲列表
            auto& list = freeSpans_[newSpan->numPages];
            newSpan->next = list;
            list = newSpan;

            // 更新原 Span 的大小
            span->numPages = numPages;
        }

        // 记录地址到 Span 的映射
        spanMap_[span->pageAddr] = span;
        return span->pageAddr;
    }

    // 没有合适的空闲 Span，需要向系统申请
    void* memory = systemAlloc(numPages);
    if (!memory) return nullptr;

    // 创建新的 Span
    Span* span = new Span;
    span->pageAddr = memory;
    span->numPages = numPages;
    span->next = nullptr;

    // 记录地址到 Span 的映射
    spanMap_[memory] = span;
    return memory;
}

/* 参数：ptr – 待释放的内存页起始地址
   参数：numPages – 内存页数量
   功能：释放内存页到页缓存，并尝试与相邻页合并
   返回值：无
   说明：
     1. 查找待释放 Span 的后继 Span，尝试合并
     2. 如果合并成功，更新 Span 信息
     3. 将空闲 Span 添加到空闲列表 */
void PageCache::deallocateSpan(void* ptr, size_t numPages)
{
    // 获取互斥锁保护
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找该地址对应的 Span
    auto it = spanMap_.find(ptr);
    if (it == spanMap_.end()) return;  // 未找到，无效的指针

    Span* span = it->second;

    // 检查是否可以与后继 Span 合并
    // 计算后继 Span 的起始地址
    void* nextAddr = static_cast<char*>(ptr) + numPages * PAGE_SIZE;
    auto nextIt = spanMap_.find(nextAddr);

    if (nextIt != spanMap_.end())
    {
        // 找到后继 Span，尝试合并
        Span* nextSpan = nextIt->second;

        // 从空闲链表中移除后继 Span
        bool found = false;
        auto& nextList = freeSpans_[nextSpan->numPages];

        if (nextList == nextSpan)
        {
            // 后继 Span 是链表头
            nextList = nextSpan->next;
            found = true;
        }
        else if (nextList)
        {
            // 在链表中查找后继 Span
            Span* prev = nextList;
            while (prev->next)
            {
                if (prev->next == nextSpan)
                {
                    prev->next = nextSpan->next;
                    found = true;
                    break;
                }
                prev = prev->next;
            }
        }

        if (found)
        {
            // 合并两个 Span
            span->numPages += nextSpan->numPages;
            spanMap_.erase(nextAddr);
            delete nextSpan;
        }
    }

    // 将合并后的 Span 添加到空闲列表
    auto& list = freeSpans_[span->numPages];
    span->next = list;
    list = span;
}

/* 参数：numPages – 需要的页数
   功能：调用系统接口分配内存
   返回值：分配的内存起始地址，失败返回 nullptr
   说明：使用 mmap 系统调用分配内存，
         MAP_ANONYMOUS 表示匿名映射，不关联任何文件 */
void* PageCache::systemAlloc(size_t numPages)
{
    // 计算需要的字节数
    size_t size = numPages * PAGE_SIZE;

    // 调用 mmap 分配内存
    // PROT_READ | PROT_WRITE: 页面可读可写
    // MAP_PRIVATE | MAP_ANONYMOUS: 私有匿名映射
    // -1: 不关联文件描述符
    // 0: 文件偏移量为 0
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return nullptr;

    // 初始化分配的内存为零
    memset(ptr, 0, size);
    return ptr;
}

} // namespace my_memoryPool_v2