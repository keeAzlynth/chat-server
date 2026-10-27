cmake_minimum_required(VERSION 3.25)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(im_chat_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add cmake/ to module search path (for FindSodium.cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# When building a fully static executable on MinGW, prefer static libs (.a)
if(MINGW)
    list(INSERT CMAKE_FIND_LIBRARY_SUFFIXES 0 .a)
endif()

# ── Dependencies ─────────────────────────────────────────
find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Sodium REQUIRED)

# Some fmt packages ship both libfmt.a and a DLL import library, and the
# imported target points at the DLL. Swap it for the archive so the executable
# does not depend on libfmt-*.dll at runtime.
find_file(IMCHAT_FMT_STATIC NAMES libfmt.a HINTS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES lib)
if(IMCHAT_FMT_STATIC AND TARGET fmt::fmt)
    add_library(imchat_fmt_static STATIC IMPORTED GLOBAL)
    set_target_properties(imchat_fmt_static PROPERTIES
        IMPORTED_LOCATION "${IMCHAT_FMT_STATIC}")
    set_target_properties(spdlog::spdlog_header_only PROPERTIES
        INTERFACE_LINK_LIBRARIES "Threads::Threads;imchat_fmt_static")
    message(STATUS "Using static fmt: ${IMCHAT_FMT_STATIC}")
endif()

# ── Main server ──────────────────────────────────────────
add_executable(im_chat_server
    src/chat_room.cpp
    src/config.cpp
    src/http_server.cpp
    src/main.cpp
    src/server.cpp
    src/session_manager.cpp
    src/thread_pool.cpp
    src/websocket_session.cpp
    src/database.cpp
)

target_include_directories(im_chat_server PRIVATE include)

target_link_libraries(im_chat_server PRIVATE
    Boost::system
    nlohmann_json::nlohmann_json
    spdlog::spdlog_header_only
    SQLite3::SQLite3
    Sodium::Sodium
)

if(WIN32)
    target_link_libraries(im_chat_server PRIVATE ws2_32 mswsock crypt32)
endif()

# Full static linking — standalone exe, no DLL dependencies (MinGW only)
if(MINGW)
    target_link_options(im_chat_server PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# ── Compile options ──────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING
        "Build type: Debug or Release" FORCE)
    message(STATUS "No build type specified — defaulting to Release")
endif()

option(ENABLE_NATIVE "Enable -march=native (Release only)" ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(im_chat_server PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-O2 -g -Wall -Wextra -Wpedantic -Wno-missing-field-initializers>
        $<$<CXX_COMPILER_ID:Clang>:-O2 -g -Wall -Wextra -Wpedantic>
        $<$<CXX_COMPILER_ID:MSVC>:/Od /Zi /W4>
    )
    target_compile_definitions(im_chat_server PRIVATE DEBUG=1)
    set_property(TARGET im_chat_server PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
else()
    set(NATIVE_FLAG "")
    if(ENABLE_NATIVE AND NOT MSVC)
        set(NATIVE_FLAG "-march=native")
    endif()
    target_compile_options(im_chat_server PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-O3 ${NATIVE_FLAG}
            -Wall -Wextra -Wpedantic -Wno-missing-field-initializers
            -fstack-protector-strong -D_FORTIFY_SOURCE=2
            -fno-strict-aliasing -fno-delete-null-pointer-checks>
        $<$<CXX_COMPILER_ID:Clang>:-O3 ${NATIVE_FLAG} -Wall -Wextra -Wpedantic>
        $<$<CXX_COMPILER_ID:MSVC>:/O2 /W4 /guard:cf>
    )
    target_compile_definitions(im_chat_server PRIVATE NDEBUG=1)
endif()

# ── Address Sanitizer (Debug only) ──────────────────────
option(ENABLE_ASAN "Enable Address Sanitizer (Debug)" OFF)
if(ENABLE_ASAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(NOT MSVC)
        target_compile_options(im_chat_server PRIVATE -fsanitize=address,undefined)
        target_link_options(im_chat_server PRIVATE -fsanitize=address,undefined)
    endif()
    message(STATUS "Address Sanitizer enabled for Debug build")
endif()

# ── Link Time Optimization (Release only) ────────────────
option(ENABLE_LTO "Enable Link Time Optimization (Release)" ON)
if(ENABLE_LTO AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_property(TARGET im_chat_server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "LTO enabled for Release build")
endif()

# --- Stress test tool ---
add_executable(stress_test
    tools/stress_test.cpp
)

target_include_directories(stress_test PRIVATE include)

target_link_libraries(stress_test PRIVATE
    Boost::system
    nlohmann_json::nlohmann_json
    SQLite3::SQLite3
    Sodium::Sodium
)

if(WIN32)
    target_link_libraries(stress_test PRIVATE ws2_32 mswsock)
endif()

if(MINGW)
    target_link_options(stress_test PRIVATE -static -static-libgcc -static-libstdc++)
endif()

target_compile_options(stress_test PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-O2 -Wno-missing-field-initializers>
    $<$<CXX_COMPILER_ID:MSVC>:/O2>
)

if(ENABLE_LTO AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_property(TARGET stress_test PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
