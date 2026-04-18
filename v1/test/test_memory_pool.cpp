#include "../include/MemoryPool.h"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <cstring>   // for memset


using namespace my_memoryPool_v1;
using namespace std::chrono;


// 测试配置
#ifdef ITERATIONS
const size_t TEST_ITERATIONS = ITERATIONS;      // 从 CMake 获取迭代次数
#else
const size_t TEST_ITERATIONS = 1000000;      // 默认每个线程分配/释放次数
#endif
const size_t THREAD_COUNTS[] = {1, 2, 4, 8}; // 测试不同线程数
const size_t MIN_SIZE = 8;
const size_t MAX_SIZE = 512;


// 记录分配的信息，用于验证和释放
struct AllocInfo {
    void* ptr;
    size_t size;
};


// 随机生成 [min, max] 范围内的 size（8 的倍数）
size_t randomSize(std::mt19937& gen, size_t min, size_t max) {
    std::uniform_int_distribution<size_t> dist(min/8, max/8);
    return dist(gen) * 8;
}


// 测试内存池分配/释放（单线程）
double testMemoryPoolSingle(size_t iterations, size_t minSize, size_t maxSize) {
    std::vector<AllocInfo> ptrs(iterations);
    std::mt19937 gen(42);  // 固定种子，便于复现


    auto start = high_resolution_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        size_t sz = randomSize(gen, minSize, maxSize);
        ptrs[i].size = sz;
        ptrs[i].ptr = HashBucket::useMemory(sz);
        if (ptrs[i].ptr) memset(ptrs[i].ptr, 0xAB, sz);
    }
    for (size_t i = 0; i < iterations; ++i) {
        HashBucket::freeMemory(ptrs[i].ptr, ptrs[i].size);
    }
    auto end = high_resolution_clock::now();
    auto elapsed = duration_cast<milliseconds>(end - start).count();
    double opsPerMs = (iterations * 1000.0 / elapsed);
    std::cout << "MemoryPool (single thread): " << elapsed << " ms, "
              << opsPerMs << " ops/ms\n";
    return static_cast<double>(elapsed);
}


// 测试 new/delete 分配/释放（单线程）
double testNewDeleteSingle(size_t iterations, size_t minSize, size_t maxSize) {
    std::vector<void*> ptrs(iterations);
    std::mt19937 gen(42);


    auto start = high_resolution_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        size_t sz = randomSize(gen, minSize, maxSize);
        ptrs[i] = operator new(sz);
        if (ptrs[i]) memset(ptrs[i], 0xAB, sz);
    }
    for (size_t i = 0; i < iterations; ++i) {
        operator delete(ptrs[i]);
    }
    auto end = high_resolution_clock::now();
    auto elapsed = duration_cast<milliseconds>(end - start).count();
    double opsPerMs = (iterations * 1000.0 / elapsed);
    std::cout << "new/delete (single thread): " << elapsed << " ms, "
              << opsPerMs << " ops/ms\n";
    return static_cast<double>(elapsed);
}


// 多线程测试函数
double testMultiThreaded(size_t numThreads, size_t iterationsPerThread,
                       size_t minSize, size_t maxSize, bool usePool) {
    std::vector<std::thread> threads;
    threads.reserve(numThreads);


    auto start = high_resolution_clock::now();
    for (size_t t = 0; t < numThreads; ++t) {
        threads.emplace_back([=]() {
            std::vector<AllocInfo> localPtrs(iterationsPerThread);
            std::mt19937 gen(t + 100);  // 每个线程不同种子
            for (size_t i = 0; i < iterationsPerThread; ++i) {
                size_t sz = randomSize(gen, minSize, maxSize);
                localPtrs[i].size = sz;
                if (usePool) {
                    localPtrs[i].ptr = HashBucket::useMemory(sz);
                } else {
                    localPtrs[i].ptr = operator new(sz);
                }
                if (localPtrs[i].ptr) memset(localPtrs[i].ptr, 0xAB, sz);
            }
            // 逆序释放
            for (size_t i = iterationsPerThread; i > 0; --i) {
                if (usePool) {
                    HashBucket::freeMemory(localPtrs[i-1].ptr, localPtrs[i-1].size);
                } else {
                    operator delete(localPtrs[i-1].ptr);
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    auto end = high_resolution_clock::now();
    auto elapsed = duration_cast<milliseconds>(end - start).count();
    size_t totalOps = numThreads * iterationsPerThread;
    double opsPerMs = (totalOps * 1000.0 / elapsed);
    std::cout << (usePool ? "MemoryPool" : "new/delete")
              << " (threads=" << numThreads << "): " << elapsed << " ms, "
              << opsPerMs << " ops/ms\n";
    return static_cast<double>(elapsed);
}


int main() {
    // 初始化内存池
    HashBucket::initMemoryPool();


    std::cout << "=== Single-thread performance (size range 8-512 bytes) ===\n";
    double poolTime = testMemoryPoolSingle(TEST_ITERATIONS, MIN_SIZE, MAX_SIZE);
    double newDelTime = testNewDeleteSingle(TEST_ITERATIONS, MIN_SIZE, MAX_SIZE);
    
    double improvement = ((newDelTime - poolTime) / newDelTime) * 100.0;
    std::cout << "Improvement: " << improvement << "% faster than new/delete\n";


    std::cout << "\n=== Multi-thread performance ===\n";
    for (size_t tc : THREAD_COUNTS) {
        double multiPoolTime = testMultiThreaded(tc, TEST_ITERATIONS / tc, MIN_SIZE, MAX_SIZE, true);
        double multiNewDelTime = testMultiThreaded(tc, TEST_ITERATIONS / tc, MIN_SIZE, MAX_SIZE, false);
        
        double multiImprovement = ((multiNewDelTime - multiPoolTime) / multiNewDelTime) * 100.0;
        std::cout << "Thread " << tc << " Improvement: " << multiImprovement << "% faster than new/delete\n";
        
        std::cout << "----------------------------\n";
    }


    return 0;
}