cmake_minimum_required(VERSION 3.10)
project(MemoryPoolSystem LANGUAGES CXX)

# 1. C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 2. Build configuration (Debug/Release)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# 3. Compile Options
if(MSVC)
    add_compile_options(/W4 -permissive- -EHsc)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

# Configuration-specific flags
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -DNDEBUG)
endif()

# 4. Output Directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 5. Dependencies (found once for all subprojects)
find_package(Threads REQUIRED)

# 6. Test parameter (used by test targets)
set(ITERATIONS "1000000" CACHE STRING "Total iterations for performance test")

# --- Version Selection Options ---
option(BUILD_V1 "Build MemoryPool v1" ON)
option(BUILD_V2 "Build MemoryPool v2" ON)

# 7. Include subprojects
if(BUILD_V1)
    add_subdirectory(v1)
endif()

if(BUILD_V2)
    add_subdirectory(v2)
endif()

# 8. Print Information
function(print_targets)
    message(STATUS "------------------------------------------------")
    message(STATUS "Project Name: ${PROJECT_NAME}")
    message(STATUS "Build Type:   ${CMAKE_BUILD_TYPE}")
    message(STATUS "Compiler:     ${CMAKE_CXX_COMPILER_ID}")
    message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
    message(STATUS "Binary Dir:   ${CMAKE_BINARY_DIR}")
    message(STATUS "Install Dir:  ${CMAKE_INSTALL_PREFIX}")
    message(STATUS "------------------------------------------------")
endfunction()

print_targets()