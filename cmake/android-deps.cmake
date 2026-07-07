# Android dependencies that vcpkg provides on other platforms.
# GeneralsX @feature android-port 06/07/2026
#
# GLM and GLI are header-only libraries that vcpkg provides on Linux/macOS/iOS.
# Android has no vcpkg, so FetchContent them. zlib is provided by the NDK.

if(ANDROID)
    include(FetchContent)

    # GLM (header-only math library)
    if(NOT TARGET glm::glm)
        message(STATUS "Configuring GLM (FetchContent for Android)...")
        FetchContent_Declare(
            glm
            GIT_REPOSITORY https://github.com/g-truc/glm.git
            GIT_TAG        1.0.1
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(glm)
        # Create the glm::glm alias that find_package(glm CONFIG) would.
        if(TARGET glm AND NOT TARGET glm::glm)
            add_library(glm::glm ALIAS glm)
        endif()
        set(glm_FOUND TRUE CACHE BOOL "" FORCE)
    endif()

    # GLI is NOT fetched: CompatLib skips it on Android (same Clang ambiguity as
    # macOS + no vcpkg config). If a future target needs it, add it here.

    message(STATUS "Android deps: GLM configured via FetchContent")
endif()
