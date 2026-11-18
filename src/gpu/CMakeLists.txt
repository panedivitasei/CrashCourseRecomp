# gpu: Crash Course's native renderer. It implements the guest-facing XDK
# D3D API directly on the plume RHI (D3D12): no PM4 command stream, no Xenia
# command processor. Ported from UnleashedRecomp (hedge-dev, GPL-3.0).
#
# LICENSE NOTE: everything in this directory is GPL-3.0 (see LICENSE here).
# It is statically linked into the game, which makes the binary subject to
# GPL-3.0.
# Namespace: rex::videonative (cvars keep their gpu_* names).

set(GPU_SOURCES
    overlay.cpp
    overlay.h
    render_queue.cpp
    renderer.cpp
    renderer.h
    renderer_fps.cpp
    renderer_fps.h
    semantics/sampler_semantics.cpp
    semantics/sampler_semantics.h
    shader_cache.cpp
    shader_cache.h
    video_native.cpp
    video_native.h
    video_native_internal.h
)

if(NOT REXGLUE_SDK_SOURCE_DIR)
    message(FATAL_ERROR "gpu: set REXGLUE_SDK_SOURCE_DIR to the SDK checkout")
endif()

add_library(gpu STATIC ${GPU_SOURCES})
add_library(cc1::gpu ALIAS gpu)

target_include_directories(gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# xxHash in header-only mode (XXH_INLINE_ALL) and renderdoc_app.h for the
# frame-capture trigger (rex/ui/renderdoc_api.h includes it unqualified).
target_include_directories(gpu PRIVATE
    ${REXGLUE_SDK_SOURCE_DIR}/thirdparty/xxHash
    ${REXGLUE_SDK_SOURCE_DIR}/thirdparty/renderdoc
    ${REXGLUE_SDK_SOURCE_DIR}/thirdparty/stb)

target_link_libraries(gpu PUBLIC plume rex::runtime)
set_target_properties(gpu PROPERTIES FOLDER "src")
