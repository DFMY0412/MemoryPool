#include "../include/ThreadCache.h"
#include "../include/CentralCache.h"
#include <cstdlib>

namespace my_memoryPool_v2 {

/* 参数：size – 请求分配的字节数
   功能：分配内存，优先从本地自由链表获取
   返回值：分配的内存地址，失败返回 nullptr
   流程：
     1. 大于 MAX_BYTES 的内存直接调用 malloc
     2. 计算大小对应的自由链表索引
     3. 从自由链表弹出一个块（无锁操作）
     4. 如果链表为空，从 CentralCache 批量获取 */
void* ThreadCache::allocate(size_t size)
{
    // size 为 0 时默认为 ALIGNMENT（8字节）
    if (size == 0)
    {
        size = ALIGNMENT;
    }

    // 大于 MAX_BYTES 的大内存直接使用系统分配
    if (size > MAX_BYTES)
    {
        return malloc(size);
    }

    // 计算大小对应的自由链表索引
    size_t index = SizeClass::getIndex(size);

    // 先递减计数（表示要取出一个块）
    freeListSize_[index]--;

    // 尝试从自由链表获取内存
    if (void* ptr = freeList_[index])
    {
        // 弹出头节点：将 freeList 指向下一个节点
        // 通过强制类型转换将内存块的前 8 字节作为 next 指针
        freeList_[index] = *reinterpret_cast<void**>(ptr);
        return ptr;
    }

    // 链表为空，从 CentralCache 批量获取
    return fetchFromCentralCache(index);
}

/* 参数：ptr – 待释放的内存指针
   参数：size – 原始请求的字节数
   功能：释放内存到本地自由链表，必要时批量归还给 CentralCache
   返回值：无
   流程：
     1. 大于 MAX_BYTES 的内存直接调用 free
     2. 计算大小对应的自由链表索引
     3. 将内存块插入自由链表头部
     4. 如果链表过长，批量归还部分给 CentralCache */
void ThreadCache::deallocate(void* ptr, size_t size)
{
    // 大于 MAX_BYTES 的大内存直接使用系统释放
    if (size > MAX_BYTES)
    {
        free(ptr);
        return;
    }

    // 计算大小对应的自由链表索引
    size_t index = SizeClass::getIndex(size);

    // 将内存块插入自由链表头部（头插法）
    // 通过强制类型转换将内存块的前 8 字节作为 next 指针
    *reinterpret_cast<void**>(ptr) = freeList_[index];
    freeList_[index] = ptr;

    // 递增计数（表示增加一个空闲块）
    freeListSize_[index]++;

    // 检查是否需要归还给 CentralCache（超过阈值 256 时触发）
    if (shouldReturnToCentralCache(index))
    {
        returnToCentralCache(freeList_[index], size);
    }
}

/* 参数：index – 自由链表索引
   功能：判断是否应该将内存归还给 CentralCache
   返回值：true 表示应该归还，false 表示保留
   说明：当某个大小类的空闲块数量超过阈值时，
         归还部分块给 CentralCache 以减少内存占用 */
bool ThreadCache::shouldReturnToCentralCache(size_t index)
{
    size_t threshold = 256;  // 阈值：每个链表最多保留 256 个空闲块
    return (freeListSize_[index] > threshold);
}

/* 参数：index – 自由链表索引
   功能：从 CentralCache 批量获取内存块
   返回值：获取的内存块起始地址
   说明：一次获取多个块，提高缓存命中率和减少锁竞争 */
void* ThreadCache::fetchFromCentralCache(size_t index)
{
    // 调用 CentralCache 获取内存块
    void* start = CentralCache::getInstance().fetchRange(index);
    if (!start) return nullptr;

    // 第一个块返回给用户
    void* result = start;

    // 将剩余块链接到自由链表
    // 通过强制类型转换将内存块的前 8 字节作为 next 指针
    freeList_[index] = *reinterpret_cast<void**>(start);

    // 统计获取的块数量
    size_t batchNum = 0;
    void* current = start;

    while (current != nullptr)
    {
        batchNum++;
        current = *reinterpret_cast<void**>(current);
    }

    // 更新空闲块计数（加上获取的批次数）
    freeListSize_[index] += batchNum;

    return result;
}

/* 参数：start – 待归还的内存块起始地址
   参数：size – 原始请求的字节数
   功能：将内存块批量归还给 CentralCache
   返回值：无
   说明：
     1. 计算需要保留和归还的块数量（保留 1/4，归还 3/4）
     2. 找到分割点，将链表分为两部分
     3. 将归还部分发送给 CentralCache */
void ThreadCache::returnToCentralCache(void* start, size_t size)
{
    size_t index = SizeClass::getIndex(size);

    // 计算对齐后的大小
    size_t alignedSize = SizeClass::roundUp(size);

    size_t batchNum = freeListSize_[index];
    if (batchNum <= 1) return;  // 只剩一个块时不归还

    // 计算保留和归还的数量：保留 1/4，归还 3/4
    size_t keepNum = std::max(batchNum / 4, size_t(1));
    size_t returnNum = batchNum - keepNum;

    // 找到第 keepNum-1 个节点，作为新的链表尾
    char* current = static_cast<char*>(start);
    char* splitNode = current;
    for (size_t i = 0; i < keepNum - 1; ++i)
    {
        splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));
        if (splitNode == nullptr)
        {
            // 链表比预期短，调整归还数量
            returnNum = batchNum - (i + 1);
            break;
        }
    }

    if (splitNode != nullptr)
    {
        // 断开链表，获取后续需要归还的部分
        void* nextNode = *reinterpret_cast<void**>(splitNode);
        *reinterpret_cast<void**>(splitNode) = nullptr;  // 断开连接

        // 更新本地链表：保留的部分
        freeList_[index] = start;
        freeListSize_[index] = keepNum;

        // 将归还部分发送给 CentralCache
        if (returnNum > 0 && nextNode != nullptr)
        {
            CentralCache::getInstance().returnRange(nextNode, returnNum * alignedSize, index);
        }
    }
}

} // namespace my_memoryPool_v2