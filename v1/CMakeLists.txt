project(MemoryPool_v1 LANGUAGES CXX)

# 1. Source and Headers
set(V1_SOURCES src/MemoryPool.cpp)
set(V1_HEADERS include/MemoryPool.h)

# 2. Add Library (Static)
add_library(memory_pool_v1 STATIC ${V1_SOURCES} ${V1_HEADERS})

# 3. Include Directories
target_include_directories(memory_pool_v1 PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/v1>
)

# 4. Add Test Executable
add_executable(test_memory_pool_v1 test/test_memory_pool.cpp)

# 5. Link Libraries (Threads is already found in root)
target_link_libraries(test_memory_pool_v1 PRIVATE 
    memory_pool_v1
    Threads::Threads
)

# 6. Define ITERATIONS macro only for the test target
target_compile_definitions(test_memory_pool_v1 PRIVATE ITERATIONS=${ITERATIONS})

# 7. Install Rules (optional, symmetric with v2)
install(TARGETS memory_pool_v1
    RUNTIME DESTINATION bin      # (not used for static libs, but kept for completeness)
    LIBRARY DESTINATION lib      # (shared libs, if any)
    ARCHIVE DESTINATION lib       # static library goes here
)

install(FILES ${V1_HEADERS} DESTINATION include/v1)