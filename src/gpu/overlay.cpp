// UI-thread side of the ImGui overlay (overlay.h): records the ImGui drawer's
// batches into a frame the render thread replays.
// GPL-3.0 - see LICENSE in this directory.

#include "overlay.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace rex::videonative::overlay {
namespace {

std::mutex g_mutex;
std::shared_ptr<const Frame> g_latest;
bool g_recorded = false;  // a frame was published since BeginFrame
std::function<void()> g_frame_callback;

class Texture final : public rex::ui::ImmediateTexture {
 public:
  explicit Texture(std::shared_ptr<TextureData> texture)
      : ImmediateTexture(texture->width, texture->height), data(std::move(texture)) {}
  std::shared_ptr<TextureData> data;
};

class Drawer final : public rex::ui::ImmediateDrawer {
 public:
  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                           rex::ui::ImmediateTextureFilter filter,
                                                           bool /*is_repeated*/,
                                                           const uint8_t* data) override {
    auto texture = std::make_shared<TextureData>();
    texture->width = std::max(width, 1u);
    texture->height = std::max(height, 1u);
    texture->linear = filter == rex::ui::ImmediateTextureFilter::kLinear;
    const size_t bytes = size_t(texture->width) * texture->height * 4;
    if (data && width && height) {
      texture->rgba.assign(data, data + bytes);
    } else {
      texture->rgba.assign(bytes, 0);
    }
    return std::make_unique<Texture>(std::move(texture));
  }

  void Begin(rex::ui::UIDrawContext& context, float width, float height) override {
    ImmediateDrawer::Begin(context, width, height);
    frame_ = std::make_shared<Frame>();
    frame_->width = width;
    frame_->height = height;
  }

  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override {
    batch_vertex_ = uint32_t(frame_->vertices.size());
    batch_index_ = uint32_t(frame_->indices.size());
    batch_indexed_ = batch.indices != nullptr;
    frame_->vertices.insert(frame_->vertices.end(), batch.vertices,
                            batch.vertices + batch.vertex_count);
    if (batch_indexed_) {
      frame_->indices.insert(frame_->indices.end(), batch.indices,
                             batch.indices + batch.index_count);
    }
  }

  // ImGui only sends indexed triangle lists.
  void Draw(const rex::ui::ImmediateDraw& draw) override {
    if (!frame_ || !batch_indexed_ || draw.count <= 0 ||
        draw.primitive_type != rex::ui::ImmediatePrimitiveType::kTriangles) {
      return;
    }
    overlay::Draw recorded;
    if (draw.texture) recorded.texture = static_cast<Texture*>(draw.texture)->data;
    recorded.index_count = uint32_t(draw.count);
    recorded.first_index = batch_index_ + uint32_t(draw.index_offset);
    recorded.base_vertex = int32_t(batch_vertex_) + draw.base_vertex;
    if (draw.scissor) {
      recorded.clip[0] = draw.scissor_left;
      recorded.clip[1] = draw.scissor_top;
      recorded.clip[2] = draw.scissor_right;
      recorded.clip[3] = draw.scissor_bottom;
    } else {
      recorded.clip[2] = frame_->width;
      recorded.clip[3] = frame_->height;
    }
    frame_->draws.push_back(std::move(recorded));
  }

  void EndDrawBatch() override {}

  void End() override {
    if (frame_) {
      std::lock_guard lock(g_mutex);
      g_latest = std::move(frame_);
      g_recorded = true;
    }
    frame_.reset();
    ImmediateDrawer::End();
  }

 private:
  std::shared_ptr<Frame> frame_;
  uint32_t batch_vertex_ = 0;
  uint32_t batch_index_ = 0;
  bool batch_indexed_ = false;
};

}  // namespace

std::unique_ptr<rex::ui::ImmediateDrawer> CreateDrawer() { return std::make_unique<Drawer>(); }

void SetFrameCallback(std::function<void()> callback) {
  std::lock_guard lock(g_mutex);
  g_frame_callback = std::move(callback);
}

void BeginFrame() {
  std::lock_guard lock(g_mutex);
  g_recorded = false;
}

void EndFrame() {
  std::lock_guard lock(g_mutex);
  if (!g_recorded) g_latest.reset();
}

std::shared_ptr<const Frame> LatestFrame() {
  std::lock_guard lock(g_mutex);
  return g_latest;
}

void NotifyPresented() {
  std::function<void()> callback;
  {
    std::lock_guard lock(g_mutex);
    callback = g_frame_callback;
  }
  if (callback) callback();
}

}  // namespace rex::videonative::overlay
