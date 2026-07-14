# GeneralsX @build fbraz 24/02/2026
# GeneralsX @bugfix fbraz 10/03/2026 Use FetchContent for ALL platforms (macOS, Linux, Windows)
# OpenAL audio library via FetchContent (openal-soft v1.24.2)
#
# On Linux, openal-soft is managed via vcpkg (see vcpkg.json). The vcpkg build compiles
# openal-soft with ALSA-only backend (no PipeWire, no PulseAudio), which avoids a SIGSEGV
# crash in the system libopenal1 1.25.1 Debian package. find_package(OpenAL) picks up the
# vcpkg-installed version automatically when the vcpkg toolchain is active.
#
# On macOS, CMake's FindOpenAL prefers Apple's deprecated OpenAL.framework which uses
# <OpenAL/al.h> instead of the standard <AL/al.h> expected by the Linux-compatible code.
# Prefer openal-soft (brew install openal-soft) which matches the Linux layout.
# Strategy: FetchContent for ALL platforms -- no Homebrew/system detection.
# - macOS:   CoreAudio backend. Compiled natively (arm64 on Apple Silicon).
#            Apple's deprecated OpenAL.framework is avoided -- it uses <OpenAL/al.h>
#            which is incompatible with the standard <AL/al.h> used throughout the codebase.
#            Homebrew openal-soft was unreliable: Intel Homebrew (/usr/local) installs
#            x86_64-only binaries that fail to link against native arm64 builds.
# - Linux:   ALSA/PipeWire backend.
# - Windows: WASAPI backend (modern, low-latency).
#
# FetchContent_MakeAvailable is idempotent: safe to include from multiple CMakeLists.
# Callers guard with: if(NOT TARGET OpenAL::OpenAL) find_package... endif()
#
# Reference: jmarshall OpenAL implementation uses <AL/al.h> throughout.

if(SAGE_USE_OPENAL)
    message(STATUS "Configuring OpenAL Soft (v1.24.2) with FetchContent...")

    include(FetchContent)

    FetchContent_Declare(
        openal_soft
        URL "https://github.com/kcat/openal-soft/archive/refs/tags/1.24.2.tar.gz"
        URL_HASH "SHA256=7efd383d70508587fbc146e4c508771a2235a5fc8ae05bf6fe721c20a348bd7c"
    )

    # Minimal build: no utilities, examples, or tests
    set(ALSOFT_INSTALL_RUNTIME_LIBS  ON  CACHE BOOL "Install runtime libs" FORCE)
    set(ALSOFT_EXAMPLES              OFF CACHE BOOL "Build examples"       FORCE)
    set(ALSOFT_TESTS                 OFF CACHE BOOL "Build tests"          FORCE)
    set(ALSOFT_UTILS                 OFF CACHE BOOL "Build utils"          FORCE)
    set(ALSOFT_NO_CONFIG_UTIL        ON  CACHE BOOL "Disable config util"  FORCE)

    if(ANDROID)
        # GeneralsX @build android-port 07/07/2026 Disable OpenAL's install/export
        # targets on Android. When the Oboe backend is enabled via
        # add_subdirectory, the oboe target is not in OpenAL's export set, which
        # breaks install(EXPORT OpenAL). We don't need install targets on Android
        # (the .so is staged into the APK manually), so disable them entirely.
        set(ALSOFT_INSTALL             OFF CACHE BOOL "Enable install"      FORCE)
        set(ALSOFT_INSTALL_RUNTIME_LIBS OFF CACHE BOOL "Install runtime libs" FORCE)
    endif()

    if(WIN32)
        # Windows: WASAPI is the modern low-latency audio API
        set(ALSOFT_REQUIRE_WASAPI ON CACHE BOOL "Require WASAPI backend on Windows" FORCE)
    endif()

    if(ANDROID)
        # GeneralsX @bugfix android-port 07/07/2026 Enable the Oboe audio backend
        # for Android. Without this, OpenAL Soft compiles with zero playback
        # backends — alcOpenDevice(NULL) returns a null device that silently
        # discards all audio. Oboe is Google's modern Android audio library
        # (AAudio on API 27+, OpenSL ES fallback on older devices).
        #
        # OpenAL Soft's CMakeLists.txt checks OBOE_SOURCE first (add_subdirectory),
        # then falls back to find_package(Oboe). We use FetchContent_Populate to
        # download the Oboe source WITHOUT creating the target ourselves — then
        # point OBOE_SOURCE at it so OpenAL's add_subdirectory owns the oboe
        # target. This avoids the "duplicate target" error.
        message(STATUS "Configuring Oboe (Android audio backend for OpenAL)...")

        FetchContent_Declare(
            oboe
            URL "https://github.com/google/oboe/archive/refs/tags/1.10.0.tar.gz"
        )
        FetchContent_Populate(oboe)

        # Patch Oboe's CMakeLists.txt to force a STATIC library.
        # Oboe uses add_library(oboe ${oboe_sources}) which inherits
        # BUILD_SHARED_LIBS=ON — producing liboboe.so. But OpenAL Soft
        # links Oboe into its own libopenal.so, which needs static symbols.
        # A shared liboboe.so causes undefined-symbol errors at link time.
        file(READ "${oboe_SOURCE_DIR}/CMakeLists.txt" _oboe_cmakelists)
        string(REPLACE "add_library(oboe \${oboe_sources})"
                       "add_library(oboe STATIC \${oboe_sources})"
                       _oboe_cmakelists "${_oboe_cmakelists}")
        file(WRITE "${oboe_SOURCE_DIR}/CMakeLists.txt" "${_oboe_cmakelists}")

        # Point OpenAL Soft's CMake at the fetched Oboe source so it does
        # add_subdirectory(${OBOE_SOURCE}) and owns the oboe target.
        set(OBOE_SOURCE "${oboe_SOURCE_DIR}" CACHE PATH "Oboe source directory" FORCE)
        set(ALSOFT_REQUIRE_OBOE ON CACHE BOOL "Require Oboe backend on Android" FORCE)

        # Patch OpenAL's Oboe backend source before FetchContent_MakeAvailable
        # compiles it. The source dir is already populated from the openal_soft
        # FetchContent_Declare above (MakeAvailable will use it).
        set(_oboe_backend_file "${CMAKE_BINARY_DIR}/_deps/openal_soft-src/alc/backends/oboe.cpp")
        if(EXISTS "${_oboe_backend_file}")
            file(READ "${_oboe_backend_file}" _oboe_backend)
            string(REPLACE "oboe::PerformanceMode::LowLatency"
                           "oboe::PerformanceMode::None"
                           _oboe_backend "${_oboe_backend}")
            file(WRITE "${_oboe_backend_file}" "${_oboe_backend}")
            message(STATUS "Patched OpenAL Oboe backend: PerformanceMode::None")
        endif()
    endif()

    FetchContent_MakeAvailable(openal_soft)

    # Force the vendored fmt 11.1.1 headers ahead of any system include dirs.
    # A Homebrew fmt (e.g. 12.x at /opt/homebrew/include) earlier on the include
    # path makes openal sources compile against fmt::v12 inline-namespace headers
    # while linking the vendored v11 static lib -> unresolved fmt::v12 symbols.
    foreach(_alsoft_tgt OpenAL alsoft.common alsoft.excommon)
        if(TARGET ${_alsoft_tgt})
            target_include_directories(${_alsoft_tgt} BEFORE PRIVATE
                "${openal_soft_SOURCE_DIR}/fmt-11.1.1/include")
        endif()
    endforeach()

    # openal-soft FetchContent creates the OpenAL::OpenAL imported target
    message(STATUS "OpenAL Soft configured: target OpenAL::OpenAL available")
endif()
