#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <cstring>
// #include <atomic>
// #include <mutex>
#include <cassert>

// 包含内存池头文件
#include "../include/MemoryPool.h"

using namespace my_memoryPool;
using namespace std::chrono;

// 测试配置
const size_t TEST_ITERATIONS = 1000000;
const size_t THREAD_COUNTS[] = {1, 2, 4, 8};
const size_t NUM_THREAD_TESTS = 4;

// 记录分配的信息，用于验证和释放
struct AllocInfo {
    void* ptr;
    size_t size;
};

// 随机生成 [8, 512] 范围内的 size（8 的倍数）
size_t randomSize(std::mt19937& gen) {
    std::uniform_int_distribution<size_t> dist(1, 64); // 1*8 to 64*8 = 8 to 512
    return dist(gen) * 8;
}

// 用于收集每个线程测试结果的結構體，並進行緩存行對齊以防止偽共享
struct alignas(CACHE_LINE_SIZE) ThreadResult {
    double elapsed_ms;
    // 填充以確保下一個實例不會與此實例共享緩存行
    char padding[CACHE_LINE_SIZE - sizeof(double)];
};

// 测试内存池分配/释放（单线程）
void testMemoryPoolSingle() {
    std::vector<AllocInfo> ptrs(TEST_ITERATIONS);
    std::mt19937 gen(42);

    auto start = high_resolution_clock::now();
    for (size_t i = 0; i < TEST_ITERATIONS; ++i) {
        size_t sz = randomSize(gen);
        ptrs[i].size = sz;
        ptrs[i].ptr = HashBucket::useMemory(sz);
        if (ptrs[i].ptr) memset(ptrs[i].ptr, 0xAB, sz);
    }
    for (size_t i = 0; i < TEST_ITERATIONS; ++i) {
        HashBucket::freeMemory(ptrs[i].ptr, ptrs[i].size);
    }
    auto end = high_resolution_clock::now();
    auto elapsed = duration_cast<milliseconds>(end - start).count();
    double opsPerMs = TEST_ITERATIONS * 1.0 / elapsed;
    std::cout << "MemoryPool (single thread): " << elapsed << " ms, "
              << opsPerMs << " ops/ms\n";
}

// 测试 new/delete 分配/释放（单线程）
void testNewDeleteSingle() {
    std::vector<void*> ptrs(TEST_ITERATIONS);
    std::mt19937 gen(42);

    auto start = high_resolution_clock::now();
    for (size_t i = 0; i < TEST_ITERATIONS; ++i) {
        size_t sz = randomSize(gen);
        ptrs[i] = operator new(sz);
        if (ptrs[i]) memset(ptrs[i], 0xAB, sz);
    }
    for (size_t i = 0; i < TEST_ITERATIONS; ++i) {
        operator delete(ptrs[i]);
    }
    auto end = high_resolution_clock::now();
    auto elapsed = duration_cast<milliseconds>(end - start).count();
    double opsPerMs = TEST_ITERATIONS * 1.0 / elapsed;
    std::cout << "new/delete (single thread): " << elapsed << " ms, "
              << opsPerMs << " ops/ms\n";
}

// 多线程测试函数 (Memory Pool)
void testMultiThreadedAccurate(size_t numThreads, double& total_time_ms) {
    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    auto start = high_resolution_clock::now();
    for (size_t t = 0; t < numThreads; ++t) {
        threads.emplace_back([=]() {
            size_t iterationsPerThread = TEST_ITERATIONS / numThreads;
            std::vector<AllocInfo> localPtrs(iterationsPerThread);
            std::mt19937 gen(static_cast<unsigned int>(t + 100)); // 每个线程不同种子
            
            for (size_t i = 0; i < iterationsPerThread; ++i) {
                size_t sz = randomSize(gen);
                localPtrs[i].size = sz;
                localPtrs[i].ptr = HashBucket::useMemory(sz);
                if (localPtrs[i].ptr) memset(localPtrs[i].ptr, 0xAB, sz);
            }
            
            for (size_t i = 0; i < iterationsPerThread; ++i) {
                HashBucket::freeMemory(localPtrs[i].ptr, localPtrs[i].size);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    auto end = high_resolution_clock::now();
    
    total_time_ms = duration_cast<milliseconds>(end - start).count();
    double opsPerMs = TEST_ITERATIONS * 1.0 / total_time_ms;
    std::cout << "MemoryPool (threads=" << numThreads << "): " << total_time_ms << " ms, "
              << opsPerMs << " ops/ms\n";
}

// 多线程 new/delete 测试函数
void testNewDeleteMultiThreadedAccurate(size_t numThreads, double& total_time_ms) {
    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    auto start = high_resolution_clock::now();
    for (size_t t = 0; t < numThreads; ++t) {
        threads.emplace_back([=]() {
            size_t iterationsPerThread = TEST_ITERATIONS / numThreads;
            std::vector<void*> localPtrs(iterationsPerThread);
            std::mt19937 gen(static_cast<unsigned int>(t + 100)); // 每个线程不同种子
            
            for (size_t i = 0; i < iterationsPerThread; ++i) {
                size_t sz = randomSize(gen);
                localPtrs[i] = operator new(sz);
                if (localPtrs[i]) memset(localPtrs[i], 0xAB, sz);
            }
            
            for (size_t i = 0; i < iterationsPerThread; ++i) {
                operator delete(localPtrs[i]);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    auto end = high_resolution_clock::now();
    
    total_time_ms = duration_cast<milliseconds>(end - start).count();
    double opsPerMs = TEST_ITERATIONS * 1.0 / total_time_ms;
    std::cout << "new/delete (threads=" << numThreads << "): " << total_time_ms << " ms, "
              << opsPerMs << " ops/ms\n";
}

int main() {
    HashBucket::initMemoryPool();

    std::cout << "=== Single-thread performance (size range 8-512 bytes) ===\n";
    testMemoryPoolSingle();
    testNewDeleteSingle();
    
    auto start_pool = high_resolution_clock::now();
    testMemoryPoolSingle();
    auto end_pool = high_resolution_clock::now();
    auto pool_time = duration_cast<milliseconds>(end_pool - start_pool).count();
    
    auto start_newdel = high_resolution_clock::now();
    testNewDeleteSingle();
    auto end_newdel = high_resolution_clock::now();
    auto newdel_time = duration_cast<milliseconds>(end_newdel - start_newdel).count();
    
    if (newdel_time > 0) {
        double improvement = ((newdel_time - pool_time) * 100.0) / newdel_time;
        std::cout << "Improvement: " << improvement << "% faster than new/delete\n";
    }

    std::cout << "\n=== Multi-thread performance ===\n";
    for (size_t i = 0; i < NUM_THREAD_TESTS; ++i) {
        size_t tc = THREAD_COUNTS[i];
        double pool_time_thread, newdel_time_thread;
        testMultiThreadedAccurate(tc, pool_time_thread);
        testNewDeleteMultiThreadedAccurate(tc, newdel_time_thread);
        
        if (newdel_time_thread > 0) {
            double improvement = ((newdel_time_thread - pool_time_thread) * 100.0) / newdel_time_thread;
            std::cout << "Thread " << tc << " Improvement: " << improvement << "% faster than new/delete\n";
        }
        
        std::cout << "----------------------------\n";
    }

    return 0;
}