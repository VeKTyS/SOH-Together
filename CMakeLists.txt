# Standalone build of the co-op code that does not depend on the game,
# so it can be unit-tested in seconds without compiling SoH.
# SoH itself is built from external/Shipwright (see docs/decisions/0001).
cmake_minimum_required(VERSION 3.24)
project(SohTogether CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ENet (MIT), see docs/decisions/0003. Its CMake target neither exports its
# include directory nor links Winsock under MSVC: both are fixed here.
FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.18
)
FetchContent_MakeAvailable(enet)
target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(enet PUBLIC ws2_32 winmm)
endif()
if(MSVC)
    # Third-party code: mixed-enum arithmetic and old Winsock name lookups.
    target_compile_options(enet PRIVATE /wd5287 /wd4996)
endif()

add_library(coop_core STATIC
    src/soh/Network/Coop/Handshake.cpp
    src/soh/Network/Coop/Transport.cpp
)
target_include_directories(coop_core PUBLIC src)
target_link_libraries(coop_core PUBLIC enet)
if(MSVC)
    target_compile_options(coop_core PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(coop_core PRIVATE -Wall -Wextra -Werror)
endif()

option(SOHT_BUILD_TESTS "Build unit tests" ON)
if(SOHT_BUILD_TESTS)
    # Same framework and version as libultraship's tests.
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.16.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(coop_tests tests/HandshakeTests.cpp tests/TransportTests.cpp)
    target_link_libraries(coop_tests PRIVATE coop_core gtest_main)
    if(MSVC)
        target_compile_options(coop_tests PRIVATE /utf-8)
    endif()
    include(GoogleTest)
    gtest_discover_tests(coop_tests)
endif()
