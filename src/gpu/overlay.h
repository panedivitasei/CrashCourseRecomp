// Dear ImGui overlay recorded on the UI thread and drawn by the render thread
// over the swap chain image at the tail of the present.
// GPL-3.0 - see LICENSE in this directory.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <rex/ui/immediate_drawer.h>

namespace rex::videonative::overlay {

// Drawer for ReXApp::OnCreateImmediateDrawer (detached mode), which keeps
// textures as pixels until the render thread uploads them.
std::unique_ptr<rex::ui::ImmediateDrawer> CreateDrawer();

// Callback run on the render thread after each present so the app draws its
// next UI frame, cleared by null.
void SetFrameCallback(std::function<void()> callback);

// UI-thread brackets around ImGuiDrawer::Draw, where a pass that records
// nothing clears the overlay.
void BeginFrame();
void EndFrame();

// ---- Render-thread side (renderer.cpp) ---------------------------------------

struct TextureData {
  uint32_t width = 0;
  uint32_t height = 0;
  bool linear = true;
  std::vector<uint8_t> rgba;  // RGBA8 rows; dropped once uploaded
};

struct Draw {
  std::shared_ptr<TextureData> texture;  // null = untextured (white)
  uint32_t index_count = 0;
  uint32_t first_index = 0;
  int32_t base_vertex = 0;
  float clip[4] = {};  // left, top, right, bottom in the frame's coordinate space
};

struct Frame {
  float width = 0.0f;  // the UI's coordinate space
  float height = 0.0f;
  std::vector<rex::ui::ImmediateVertex> vertices;
  std::vector<uint16_t> indices;
  std::vector<Draw> draws;
};

// The newest recorded frame, or null when the UI drew nothing.
std::shared_ptr<const Frame> LatestFrame();

// Render thread, after a present: asks the app for the next UI frame.
void NotifyPresented();

}  // namespace rex::videonative::overlay
