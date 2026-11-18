# cc1 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake. Builds against an unmodified
# ReXGlue SDK source tree: -DREXSDK_DIR=<path to rexglue-sdk>.

cmake_minimum_required(VERSION 3.25)
project(cc1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources: the app shell, CC1's hooks into the recompiled game, and the kernel
# pieces the title owns (saves, avatars). Those compile into the exe so their
# __imp__ definitions replace the runtime's at link time.
file(GLOB AVATAR_LIB_SOURCES src/kernel/xam/avatars/*.cpp)
set(CC1_SOURCES
    src/main.cpp
    src/fighter_step.cpp
    src/game_lua.cpp
    src/guest_hooks.cpp
    src/game_text.cpp
    src/input_assignment.cpp
    src/controls.cpp
    src/content_seed.cpp
    src/dll_dir.cpp
    src/legacy_sounds.cpp
    src/gamer_pictures.cpp
    src/intro_skip.cpp
    src/tick.cpp
    src/achievements.cpp
    src/spectator.cpp
    src/puppet.cpp
    src/zoom.cpp
    src/local_players.cpp
    src/mouse.cpp
    src/profile_settings.cpp
    src/invite.cpp
    src/kernel_hooks.cpp
    src/online.cpp
    src/online/leaderboards.cpp
    src/online/lobby_pose.cpp
    src/online/relay.cpp
    src/online/xam_live.cpp
    src/online/xam_net.cpp
    src/probe.cpp
    src/save_key.cpp
    src/settings.cpp
    src/usbcam.cpp
    src/video_hooks.cpp
    src/xam_content.cpp
    src/kernel/xam/xam_avatar.cpp
    ${AVATAR_LIB_SOURCES}
)

if(WIN32)
    add_executable(cc1 WIN32 ${CC1_SOURCES})
    # Windows resources: app icon (MAINICON, loaded by the runtime's window
    # class) + version block (Task Manager display name). PRECOMPILED from
    # src/crashcourse1.rc (CMake's clang RC pipeline is broken); regenerate:
    #   rc.exe /nologo /fo src/crashcourse1.res src/crashcourse1.rc
    target_link_options(cc1 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/crashcourse1.res")
else()
    add_executable(cc1 ${CC1_SOURCES})
endif()

# The exe keeps the name its config, installer and shortcuts know.
set_target_properties(cc1 PROPERTIES OUTPUT_NAME "crashcourse1")

rexglue_setup_target(cc1)

# glm for the avatar decode library (src/kernel/xam/avatars).
target_include_directories(cc1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/glm)

# ImGui headers for the app shell and overlays. Include dirs only: the objects live
# in the runtime, which exports them.
target_include_directories(cc1 PRIVATE
    $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)

# Native renderer (src/gpu, GPL-3.0) on the plume RHI (MIT, thirdparty/plume).
# The hook table in src/video_hooks.cpp routes the XDK D3D calls to it when the
# gpu_native cvar is on. It borrows a few header-only libraries from the SDK
# tree (xxHash, renderdoc_app.h, stb_image.h).
if(NOT REXSDK_DIR)
    message(FATAL_ERROR "Set -DREXSDK_DIR=<path to the ReXGlue SDK source tree>")
endif()
set(REXGLUE_SDK_SOURCE_DIR "${REXSDK_DIR}")
add_subdirectory(thirdparty/plume plume)
add_subdirectory(src/gpu)
target_link_libraries(cc1 PRIVATE cc1::gpu)

# JMlive: the Guide with Game Jolt sign-in. TITLE_INPUT leaves XamInputGetState to
# src/kernel_hooks.cpp, which hands each pad read to the Guide. The checkout sits
# beside this repo (or beside CrashCourse1/ in the dev tree).
if(NOT JMLIVE_DIR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../jmlive/jmlive/jmlive.cmake")
        set(JMLIVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../jmlive")
    else()
        set(JMLIVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../jmlive")
    endif()
endif()
if(NOT EXISTS "${JMLIVE_DIR}/jmlive/jmlive.cmake")
    message(FATAL_ERROR "JMlive not found at ${JMLIVE_DIR}; set -DJMLIVE_DIR=<path to jmlive>")
endif()
include("${JMLIVE_DIR}/jmlive/jmlive.cmake")
jmlive_attach(cc1 TITLE_INPUT)

# SDL's controller mappings, read from beside the exe (hid_mappings_file): any pad in the database works.
add_custom_command(TARGET cc1 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/SDL_GameControllerDB/gamecontrollerdb.txt" "$<TARGET_FILE_DIR:cc1>")

# The mouse pointer's cursor is set from a subclass of the window (src/mouse.cpp).
if(WIN32)
    target_link_libraries(cc1 PRIVATE comctl32)
endif()

# The runtime's libraries load from bin\ beside the exe when it is there (src/dll_dir.cpp).
if(WIN32)
    foreach(_cc1_library rexruntime TracyClient)
        if(TARGET ${_cc1_library})
            target_link_options(cc1 PRIVATE "LINKER:/DELAYLOAD:$<TARGET_FILE_NAME:${_cc1_library}>")
        endif()
    endforeach()
    target_link_libraries(cc1 PRIVATE delayimp)
endif()

# The config is read from the executable's folder.
if(NOT EXISTS "${CMAKE_BINARY_DIR}/crashcourse1.toml")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/crashcourse1.toml"
                   "${CMAKE_BINARY_DIR}/crashcourse1.toml" COPYONLY)
endif()
