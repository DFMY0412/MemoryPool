#pragma once
#include "ThreadCache.h"

namespace my_memoryPool_v2 {

/* 内存池统一接口类
   提供静态方法供外部调用，封装了 ThreadCache 的分配和释放操作
   作为内存池的入口点，用户无需关心底层实现细节 */
class MemoryPool
{
public:
    /* 参数：size – 请求分配的字节数
       功能：从内存池分配指定大小的内存
       返回值：分配的内存地址，失败返回 nullptr */
    static void* allocate(size_t size)
    {
        // 委托给 ThreadCache 进行实际分配
        // ThreadCache 采用线程本地存储，实现无锁分配
        return ThreadCache::getInstance()->allocate(size);
    }

    /* 参数：ptr – 待释放的内存指针
       参数：size – 原始请求的字节数（必须与分配时一致）
       功能：将内存归还到内存池
       返回值：无 */
    static void deallocate(void* ptr, size_t size)
    {
        // 委托给 ThreadCache 进行实际释放
        ThreadCache::getInstance()->deallocate(ptr, size);
    }
};

} // namespace my_memoryPool_v2