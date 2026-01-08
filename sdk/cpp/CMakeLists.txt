cmake_minimum_required(VERSION 3.16)

# Allow older cmake_minimum_required in fetched projects
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum policy version for compatibility")

project(lx VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(LX_BUILD_EXAMPLES "Build example applications" ON)
option(LX_BUILD_TESTS "Build tests" OFF)

# Find dependencies
find_package(Threads REQUIRED)

# Include FetchContent for external dependencies
include(FetchContent)

# nlohmann/json
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# websocketpp - header-only, just need source dir
FetchContent_Declare(
    websocketpp
    GIT_REPOSITORY https://github.com/zaphoyd/websocketpp.git
    GIT_TAG 0.8.2
)
FetchContent_Populate(websocketpp)

# asio (standalone, no Boost)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-28-2
)
FetchContent_Populate(asio)

# Library target
add_library(lx
    src/client.cpp
    src/orderbook.cpp
)

target_include_directories(lx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${websocketpp_SOURCE_DIR}
        ${asio_SOURCE_DIR}/asio/include
)

target_link_libraries(lx
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        Threads::Threads
)

target_compile_definitions(lx
    PRIVATE
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
)

# Platform-specific settings
if(APPLE)
    target_compile_definitions(lx PRIVATE _DARWIN_C_SOURCE)
endif()

if(WIN32)
    target_link_libraries(lx PRIVATE ws2_32)
endif()

# Examples
if(LX_BUILD_EXAMPLES)
    add_executable(lx_basic examples/basic.cpp)
    target_link_libraries(lx_basic PRIVATE lx)
    target_include_directories(lx_basic
        PRIVATE
            ${websocketpp_SOURCE_DIR}
            ${asio_SOURCE_DIR}/asio/include
    )
    target_compile_definitions(lx_basic
        PRIVATE
            ASIO_STANDALONE
            _WEBSOCKETPP_CPP11_STL_
    )
endif()

# Install
install(TARGETS lx
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
)
