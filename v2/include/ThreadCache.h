#pragma once
#include "Common.h"

namespace my_memoryPool_v2 {

/* 线程本地缓存类
   每个线程拥有独立的实例，实现无锁的内存分配和释放
   通过 thread_local 存储确保线程隔离，减少锁竞争 */
class ThreadCache
{
public:
    /* 功能：获取 ThreadCache 实例（线程单例）
       返回值：当前线程的 ThreadCache 实例指针 */
    static ThreadCache* getInstance()
    {
        // thread_local 确保每个线程有独立的实例
        // 静态局部变量保证只初始化一次（C++11 线程安全）
        static thread_local ThreadCache instance;
        return &instance;
    }

    /* 参数：size – 请求分配的字节数
       功能：分配内存，优先从本地自由链表获取
       返回值：分配的内存地址，失败返回 nullptr */
    void* allocate(size_t size);

    /* 参数：ptr – 待释放的内存指针
       参数：size – 原始请求的字节数
       功能：释放内存到本地自由链表，必要时归还给 CentralCache
       返回值：无 */
    void deallocate(void* ptr, size_t size);

private:
    // 私有构造函数，确保不能直接创建实例，必须通过 getInstance()
    ThreadCache()
    {
        // 初始化所有自由链表为空
        freeList_.fill(nullptr);
        // 初始化所有自由链表的计数为 0
        freeListSize_.fill(0);
    }

    /* 参数：index – 自由链表索引
       功能：从 CentralCache 批量获取内存块
       返回值：获取的内存块起始地址 */
    void* fetchFromCentralCache(size_t index);

    /* 参数：start – 待归还的内存块起始地址
       参数：size – 原始请求的字节数
       功能：将内存块归还给 CentralCache（批量归还）
       返回值：无 */
    void returnToCentralCache(void* start, size_t size);

    /* 参数：index – 自由链表索引
       功能：判断是否应该将内存归还给 CentralCache
       返回值：true 表示应该归还，false 表示保留 */
    bool shouldReturnToCentralCache(size_t index);

private:
    // 自由链表数组，每个索引对应一种大小类的空闲内存块链表
    std::array<void*, FREE_LIST_SIZE>  freeList_;

    // 自由链表的长度数组，记录每个链表中的空闲块数量
    std::array<size_t, FREE_LIST_SIZE> freeListSize_;
};

} // namespace my_memoryPool_v2