project(MemoryPool_v2 LANGUAGES CXX)

# 1. Source and Headers
set(V2_SOURCES src/MemoryPool.cpp)
set(V2_HEADERS include/MemoryPool.h)

# 2. Add Library (Static)
add_library(memory_pool_v2 STATIC ${V2_SOURCES} ${V2_HEADERS})

# 3. Include Directories
target_include_directories(memory_pool_v2 PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/v2>
)

# 4. Add Test Executable
add_executable(test_memory_pool_v2 test/test_memory_pool.cpp)

# 5. Link Libraries
target_link_libraries(test_memory_pool_v2 PRIVATE 
    memory_pool_v2
    Threads::Threads
)

# 6. Define ITERATIONS macro only for the test target
target_compile_definitions(test_memory_pool_v2 PRIVATE ITERATIONS=${ITERATIONS})

# 7. Install Rules (install only the library, not the test)
install(TARGETS memory_pool_v2
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES ${V2_HEADERS} DESTINATION include/v2)

# (Optional: uncomment to also install the test executable)
# install(TARGETS test_memory_pool_v2 RUNTIME DESTINATION bin)