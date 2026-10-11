# language: CMake, file: wisp-code/CMakeLists.txt
cmake_minimum_required(VERSION 3.15)

project(wisp-code-free C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# ------------------------------------------------------------------
# Threads: on POSIX this finds pthread; on Windows the compat header
# compat/wisp_threads.h provides Win32 primitives directly.
# ------------------------------------------------------------------
if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

# ------------------------------------------------------------------
# OpenSSL
# ------------------------------------------------------------------
if(WIN32)
    find_package(OpenSSL QUIET)
    if(NOT OpenSSL_FOUND)
        file(GLOB _openssl_dirs
            "$ENV{ProgramFiles}/TaurusTLS Developers/OpenSSL-*"
            "$ENV{ProgramFiles}/OpenSSL-Win64"
            "$ENV{ProgramFiles}/OpenSSL-Win32"
            "$ENV{ProgramFiles}/FireDaemon OpenSSL*"
        )
        foreach(_d ${_openssl_dirs})
            if(EXISTS "${_d}/include/openssl/ssl.h")
                set(OPENSSL_ROOT_DIR "${_d}")
                message(STATUS "wisp-code: found OpenSSL at ${_d}")
                find_package(OpenSSL REQUIRED)
                break()
            endif()
        endforeach()

        if(NOT OpenSSL_FOUND)
            message(FATAL_ERROR
"================================================================
  OpenSSL was not found on this system.

  wisp-code needs OpenSSL for its HTTPS transport. Install it
  once, then re-run CMake.

  --- Fastest: winget (built into Windows 10 1809+ / 11) --------
  In PowerShell:

    winget install --id FireDaemon.OpenSSL --exact --silent --accept-package-agreements --accept-source-agreements

  The installer adds OpenSSL to PATH. Close this shell, open a
  new one, and re-run cmake.

  --- Alternative: direct download ----------------------------
  https://www.firedaemon.com/firedaemon-openssl

  Download the x64 EXE and run:

    FireDaemon-OpenSSL-x64-*.exe /exenoui /qn /norestart REBOOT=ReallySuppress ADJUSTSYSTEMPATHENV=yes
================================================================
")
        endif()
    endif()
else()
    find_package(OpenSSL REQUIRED)
endif()

# ------------------------------------------------------------------
# Detect Termux — CMAKE_SYSTEM_NAME says "Linux", uname -o says
# "Android". Needed so we can pick the right shell path.
# ------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    execute_process(
        COMMAND uname -o
        OUTPUT_VARIABLE WISP_UNAME_O
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(WISP_UNAME_O MATCHES "Android")
        set(WISP_TERMUX TRUE)
        message(STATUS "wisp-code: targeting Termux/Android")
    endif()
endif()

# ------------------------------------------------------------------
# Mongoose build config
# ------------------------------------------------------------------
add_compile_definitions(
    MG_TLS=MG_TLS_OPENSSL
    MG_ENABLE_SOCKET=1
    MG_ENABLE_LOG=0
)

# ------------------------------------------------------------------
# Target
# ------------------------------------------------------------------
add_executable(wisp-code
    port.c
    console.c
    project.c
    http.c
    reply.c
    tui.c
    platform.c
    theme.c
    settings.c
    highlight.c
    mongoose.c
    cJSON.c
)

target_include_directories(wisp-code PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(WIN32)
    target_include_directories(wisp-code PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/compat
    )
endif()

# ------------------------------------------------------------------
# Libraries
# ------------------------------------------------------------------
if(WIN32)
    target_link_libraries(wisp-code PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        ws2_32
        shell32
        advapi32
        user32
    )
else()
    target_link_libraries(wisp-code PRIVATE
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
    )
endif()

# ------------------------------------------------------------------
# Compile options
# ------------------------------------------------------------------
if(WIN32)
    target_compile_definitions(wisp-code PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
    )

    if(MSVC)
        target_compile_options(wisp-code PRIVATE /W4 /wd4100 /utf-8)
    else()
        target_compile_options(wisp-code PRIVATE -Wall -Wextra)
    endif()

    # Enable RC compilation on non-MSVC toolchains (MinGW, Clang, etc.).
    # MSVC enables it automatically.
    if(NOT MSVC)
        enable_language(RC)
    endif()

    target_sources(wisp-code PRIVATE wisp-code.rc)        

elseif(WISP_TERMUX)
    target_compile_options(wisp-code PRIVATE
        -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation)
    target_compile_definitions(wisp-code PRIVATE WISP_TERMUX=1)

elseif(APPLE)
    target_compile_options(wisp-code PRIVATE
        -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation)

elseif(UNIX)
    target_compile_options(wisp-code PRIVATE
        -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation)
endif()