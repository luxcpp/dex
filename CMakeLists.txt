cmake_minimum_required(VERSION 3.16)
project(luxdex VERSION 1.0.0 LANGUAGES CXX)

# C++17 required for shared_mutex, structured bindings, etc.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type defaults to Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler-specific optimizations
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -flto")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")

    # Warning flags
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Werror=return-type
        -Werror=unused-result
        -Wno-unused-parameter
    )
elseif(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG /GL")
    add_compile_options(/W4)
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Source files
set(LUXDEX_SOURCES
    src/orderbook.cpp
    src/matching.cpp
    src/engine.cpp
    src/oracle.cpp
    src/book.cpp
    src/pool.cpp
    src/vault.cpp
    src/feed.cpp
    src/lx.cpp
)

# Header files (for IDE integration)
set(LUXDEX_HEADERS
    include/lux/order.hpp
    include/lux/trade.hpp
    include/lux/orderbook.hpp
    include/lux/engine.hpp
    include/lux/oracle.hpp
    include/lux/types.hpp
    include/lux/book.hpp
    include/lux/pool.hpp
    include/lux/vault.hpp
    include/lux/feed.hpp
    include/lux/lx.hpp
)

# Shared library
add_library(luxdex SHARED ${LUXDEX_SOURCES} ${LUXDEX_HEADERS})
target_include_directories(luxdex PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Static library (for CGO linking)
add_library(luxdex_static STATIC ${LUXDEX_SOURCES} ${LUXDEX_HEADERS})
target_include_directories(luxdex_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(luxdex_static PROPERTIES OUTPUT_NAME luxdex)

# Platform-specific threading
find_package(Threads REQUIRED)
target_link_libraries(luxdex PRIVATE Threads::Threads)
target_link_libraries(luxdex_static PRIVATE Threads::Threads)

# Set library version
set_target_properties(luxdex PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

# Position independent code for shared library
set_target_properties(luxdex PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(luxdex_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Export symbols for shared library
include(GenerateExportHeader)
generate_export_header(luxdex
    BASE_NAME LUXDEX
    EXPORT_FILE_NAME ${CMAKE_BINARY_DIR}/include/lux/export.hpp
)

# C API for FFI/CGO bindings
add_library(luxdex_c SHARED
    ${LUXDEX_SOURCES}
    bindings/c/luxdex_c.cpp
)
target_include_directories(luxdex_c PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/bindings/c>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(luxdex_c PRIVATE Threads::Threads)
set_target_properties(luxdex_c PROPERTIES
    C_VISIBILITY_PRESET default
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)

# C API header generation directory
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/bindings/c)

# Full C API for complete LX stack (Pool, Book, Vault, Oracle, Feed)
add_library(lx_full_c SHARED
    ${LUXDEX_SOURCES}
    bindings/c/lx_full_c.cpp
)
target_include_directories(lx_full_c PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/bindings/c>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(lx_full_c PRIVATE Threads::Threads)
set_target_properties(lx_full_c PROPERTIES
    C_VISIBILITY_PRESET default
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)

# Test executable
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()

    add_executable(luxdex_test
        test/test_main.cpp
    )
    target_link_libraries(luxdex_test PRIVATE luxdex_static Threads::Threads)
    target_include_directories(luxdex_test PRIVATE ${CMAKE_SOURCE_DIR}/include)

    add_test(NAME luxdex_test COMMAND luxdex_test)
endif()

# Benchmark executable
option(BUILD_BENCHMARKS "Build benchmarks" OFF)
if(BUILD_BENCHMARKS)
    add_executable(luxdex_bench
        bench/bench_main.cpp
    )
    target_link_libraries(luxdex_bench PRIVATE luxdex_static Threads::Threads)
    target_include_directories(luxdex_bench PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()

# Install targets
install(TARGETS luxdex luxdex_static luxdex_c lx_full_c
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/lux
    DESTINATION include
)

install(FILES ${CMAKE_BINARY_DIR}/include/lux/export.hpp
    DESTINATION include/lux
)

# Package configuration
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/luxdexConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# Print configuration summary
message(STATUS "")
message(STATUS "luxdex ${PROJECT_VERSION} configuration:")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Tests: ${BUILD_TESTS}")
message(STATUS "  Benchmarks: ${BUILD_BENCHMARKS}")
message(STATUS "")
