#pragma once
#include <cstddef>
#include <atomic>
#include <array>

namespace my_memoryPool_v2 {

// 对齐数和大小定义
constexpr size_t ALIGNMENT = 8;                        // 内存对齐字节数（8字节对齐）
constexpr size_t MAX_BYTES = 256 * 1024;                // 单次分配的最大字节数（256KB）
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; // 自由链表数量（32768个）

/* 内存块头部信息（预留接口，当前版本未使用）
   用于管理内存块的元数据，包括大小、使用状态和链表指针 */
struct BlockHeader
{
    size_t size;          // 内存块大小
    bool   inUse;         // 使用标志（true表示已分配，false表示空闲）
    BlockHeader* next;    // 指向下一个内存块（用于自由链表）
};

/* 大小类管理类
   负责内存大小的对齐计算和自由链表索引计算 */
class SizeClass
{
public:
    /* 参数：bytes – 原始请求的字节数
       功能：将字节数向上对齐到 ALIGNMENT 的倍数
       返回值：对齐后的字节数 */
    static size_t roundUp(size_t bytes)
    {
        // 例如：bytes=10, ALIGNMENT=8 时
        // (10 + 7) & ~7 = 17 & ~7 = 16
        return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    }

    /* 参数：bytes – 原始请求的字节数
       功能：计算请求大小对应的自由链表索引
       返回值：自由链表索引（0 ~ FREE_LIST_SIZE-1）*/
    static size_t getIndex(size_t bytes)
    {
        // 确保bytes至少为ALIGNMENT，避免索引计算错误
        bytes = std::max(bytes, ALIGNMENT);
        // 向上取整后-1，得到索引值
        // 例如：bytes=8 -> (8+7)/8-1 = 0
        //       bytes=16 -> (16+7)/8-1 = 2
        return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
    }
};

} // namespace my_memoryPool_v2