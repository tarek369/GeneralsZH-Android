# Freetype font library for Android (FetchContent, no vcpkg)
# GeneralsX @feature android-port 06/07/2026
#
# iOS/macOS/Linux get freetype via vcpkg. Android has no vcpkg path, so build
# it from source via FetchContent — the same approach used for SDL3/OpenAL.
# Freetype has a CMake build that cross-compiles cleanly with the NDK.

if(ANDROID AND NOT TARGET Freetype::Freetype)
    include(FetchContent)
    message(STATUS "Configuring FreeType (v2.13.3) with FetchContent for Android...")

    set(FREETYPE_VERSION "2.13.3")
    FetchContent_Declare(
        freetype
        GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
        GIT_TAG        VER-2-13-3
        GIT_SHALLOW    TRUE
    )

    # Freetype options: disable everything we don't need (the engine only uses
    # the core glyph-rasterization API). This shrinks the build and avoids
    # pulling in optional deps (bzip2, harfbuzz, brotli, png, zlib).
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_ENABLE_ERROR_STRINGS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(freetype)

    # Create the Freetype::Freetype alias target that find_package(Freetype) would.
    if(TARGET freetype AND NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype ALIAS freetype)
    endif()

    # GeneralsX @build android-port 06/07/2026
    # Populate the FREETYPE_* cache variables so CMake's FindFreetype.cmake
    # (invoked by find_package(Freetype REQUIRED) in WW3D2) finds them instead
    # of searching system paths. FetchContent's target handles the actual link;
    # these variables just satisfy the find_package check.
    set(FREETYPE_FOUND TRUE CACHE BOOL "Freetype found (FetchContent)" FORCE)
    set(FREETYPE_INCLUDE_DIRS "${freetype_SOURCE_DIR}/include" CACHE PATH "" FORCE)
    set(FREETYPE_LIBRARIES "Freetype::Freetype" CACHE STRING "" FORCE)
    # Also set the variables FindFreetype's _FPHSA checks for.
    set(Freetype_FOUND TRUE CACHE BOOL "" FORCE)

    message(STATUS "FreeType for Android configured (FetchContent)")
endif()
