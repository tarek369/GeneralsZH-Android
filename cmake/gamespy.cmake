set(GS_OPENSSL FALSE)
set(GAMESPY_SERVER_NAME "server.cnc-online.net")

FetchContent_Declare(
    gamespy
    GIT_REPOSITORY https://github.com/TheAssemblyArmada/GamespySDK.git
    GIT_TAG        07e3d15c500415abc281efb74322ab6d9c857eb8
)

FetchContent_MakeAvailable(gamespy)

# GeneralsX @feature android-port 06/07/2026
# Android's bionic libc does NOT provide pthread_cancel (removed — Android uses
# pthread_kill + cooperative cancellation). GameSpy's gsiCancelThread calls it.
# Provide a no-op stub + allow the implicit declaration (the engine's LAN/online
# play rarely cancels threads; when it does the thread runs to completion).
if(ANDROID)
    # Write a stub C file that defines pthread_cancel as a no-op returning 0.
    file(WRITE "${CMAKE_BINARY_DIR}/android_pthread_cancel_stub.c"
         "/* Android bionic lacks pthread_cancel; no-op stub for GameSpy. */\n"
         "int pthread_cancel(int thread) { (void)thread; return 0; }\n")
    # Add the stub to every gamespy target that links gscommon's objects.
    # Also relax implicit-function-declaration so the call compiles.
    foreach(_t gscommon gschat gsp2p gsqr2 gswebservice gsserverbrowser)
        if(TARGET ${_t})
            target_compile_options(${_t} PRIVATE -Wno-error=implicit-function-declaration -Wno-implicit-function-declaration)
        endif()
    endforeach()
    # Define the stub as a real object library that the engine + gamespy link.
    add_library(android_pthread_cancel STATIC ${CMAKE_BINARY_DIR}/android_pthread_cancel_stub.c)
    # Link the stub into the gamespy .so so the unresolved pthread_cancel resolves.
    if(TARGET gamespy)
        target_link_libraries(gamespy PRIVATE android_pthread_cancel)
    endif()
endif()
