// Native custom-avatar substitution (overview in custom_avatar.h): builds
// host-side Model structures from the custom mesh and lets the serializers in
// guest_load_asset.cpp produce the guest bytes.

#include "custom_avatar.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>

#include "guest_asset.h"
#include "model.h"
#include "texture.h"

#include "xe_compat.h"

// Master switch for the native custom-avatar substitution.
REXCVAR_DEFINE_BOOL(dcc2_custom_avatar_native, true, "Kernel",
                    "Render the avatar editor's active custom avatar through the game's "
                    "native avatar pipeline (substituted in XamAvatarGetAssets)");

// Reverses the custom OBJ's triangle winding when it is opposite the default
// avatar's under the native pipeline's cull mode.
REXCVAR_DEFINE_BOOL(dcc2_custom_avatar_flip_winding, false, "Kernel",
                    "Flips custom-avatar triangle winding, for a native custom avatar "
                    "that renders inside-out.");

// Debug aid that packs every custom-avatar normal as +Y to separate a normals
// problem from a material-parameter problem in lighting.
REXCVAR_DEFINE_BOOL(dcc2_custom_avatar_debug_normals, false, "Kernel",
                    "Forces all custom-avatar normals to +Y for uniform lighting.");

// game_data_root is defined in the runtime (runtime.cpp) and locates
// <game_data_root>/avatars/.
REXCVAR_DECLARE(std::string, game_data_root);

namespace rex {
namespace avatars {

namespace {

// --- cached custom-avatar state ----------------------------------------------

struct RgbaImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;  // host RGBA8, row 0 = top
};

// One batch per material group that owns at least one triangle.
struct CustomBatch {
  int texture_slot = 0;                 // index into CustomAvatarState::textures
  std::vector<uint32_t> src_vertex;     // per output vertex: OBJ position index
  std::vector<Vector2<uint16_t>> uvs;   // per output vertex: guest half2 uv
  std::vector<uint16_t> indices;        // triangle list (3 per tri)
};

struct CustomAvatarState {
  bool tried = false;
  bool loaded = false;
  bool from_baked = false;
  bool warned = false;   // one-shot failure warning
  std::string name;

  std::vector<float> pos;      // xyz per OBJ position vertex (auto-fit mutates)
  std::vector<float> normals;  // unit xyz per OBJ position vertex (packed at build)
  std::vector<CustomBatch> batches;
  std::vector<RgbaImage> textures;  // slot 0 = shared white fallback

  // Weight transfer results (per OBJ position vertex), filled on first
  // ApplyCustomAvatar against the decoded default components.
  bool transferred = false;
  bool fitted = false;
  std::vector<uint32_t> blend_weight;
  std::vector<uint32_t> blend_indices;
};

std::mutex g_mutex;
CustomAvatarState g_state;

void WarnOnce(const std::string& msg) {
  if (!g_state.warned) {
    g_state.warned = true;
    REXKRNL_WARN("[customavatar] {} -- keeping the default avatar", msg);
  }
}

std::string TrimCopy(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.erase(s.begin());
  }
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' ||
                        s.back() == '\t')) {
    s.pop_back();
  }
  return s;
}

// 11:11:10 signed-normalized normal pack (shader decodes ibfe 11/11/10 then
// scales by 1/1023, 1/1023, 1/511).
uint32_t PackNormal(float x, float y, float z) {
  auto q11 = [](float v) {
    int i = static_cast<int>(std::lround(v * 1023.0f));
    i = std::min(1023, std::max(-1023, i));
    return static_cast<uint32_t>(i) & 0x7FFu;
  };
  auto q10 = [](float v) {
    int i = static_cast<int>(std::lround(v * 511.0f));
    i = std::min(511, std::max(-511, i));
    return static_cast<uint32_t>(i) & 0x3FFu;
  };
  return q11(x) | (q11(y) << 11) | (q10(z) << 22);
}

// Loads a "<png path>.rgba" sidecar (uint32 w, uint32 h, then top-down RGBA
// rows) written by the avatar editor, returning false if absent or invalid.
bool LoadRgbaSidecar(const std::string& png_path, RgbaImage& out) {
  std::ifstream f(png_path + ".rgba", std::ios::binary);
  if (!f) {
    return false;
  }
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
  if (d.size() < 8) {
    return false;
  }
  uint32_t w = 0, h = 0;
  std::memcpy(&w, d.data(), 4);
  std::memcpy(&h, d.data() + 4, 4);
  if (!w || !h || static_cast<size_t>(w) * h * 4u + 8u > d.size()) {
    return false;
  }
  out.width = w;
  out.height = h;
  out.rgba.assign(d.begin() + 8, d.begin() + 8 + static_cast<size_t>(w) * h * 4u);
  return true;
}

// Nearest-neighbor resample so the width is a multiple of 128 texels, matching
// the serializer's 512-byte row padding, and both dimensions stay <= 1024, the
// stock avatar texture limit.
void NormalizeImage(RgbaImage& img) {
  auto pick_w = [](uint32_t w) {
    uint32_t t = ((w + 127u) / 128u) * 128u;
    return std::min(1024u, std::max(128u, t));
  };
  uint32_t nw = pick_w(img.width);
  uint32_t nh = std::min(1024u, img.height);
  if (nw == img.width && nh == img.height) {
    return;
  }
  std::vector<uint8_t> out(static_cast<size_t>(nw) * nh * 4u);
  for (uint32_t y = 0; y < nh; ++y) {
    uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(y) * img.height) / nh);
    for (uint32_t x = 0; x < nw; ++x) {
      uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(x) * img.width) / nw);
      const uint8_t* s = &img.rgba[(static_cast<size_t>(sy) * img.width + sx) * 4u];
      uint8_t* p = &out[(static_cast<size_t>(y) * nw + x) * 4u];
      p[0] = s[0];
      p[1] = s[1];
      p[2] = s[2];
      p[3] = s[3];
    }
  }
  img.width = nw;
  img.height = nh;
  img.rgba.swap(out);
}

// --- OBJ / MTL loading (mirrors sentient.cpp DCC2_LoadCustomAvatar) ----------

// Loads active.txt -> model_baked.obj (fallback model.obj) + .mtl + .rgba
// sidecars into g_state, with g_mutex held by the caller.
void LoadCustomAvatarLocked() {
  g_state.tried = true;
  g_state.loaded = false;

  const std::string root = REXCVAR_GET(game_data_root);
  if (root.empty()) {
    return;  // silent: no game data root yet
  }
  const std::string base = root + "/avatars/";
  std::string name;
  {
    std::ifstream af(base + "active.txt");
    if (!af) {
      return;  // silent: feature not configured
    }
    std::getline(af, name);
  }
  name = TrimCopy(name);
  if (name.empty()) {
    return;
  }
  g_state.name = name;

  // Prefers the editor's aligned output (model_baked.obj) over model.obj.
  std::string obj_file = base + name + "/model_baked.obj";
  bool used_baked = true;
  {
    std::ifstream probe(obj_file);
    if (!probe) {
      obj_file = base + name + "/model.obj";
      used_baked = false;
    }
  }
  std::ifstream obj(obj_file);
  if (!obj) {
    WarnOnce("active custom avatar '" + name + "' has no model_baked.obj/model.obj");
    return;
  }
  const std::string dir = base + name + "/";

  std::vector<float> pos;              // OBJ v (3 per vertex)
  std::vector<float> uvs;              // OBJ vt (2 per entry)
  std::vector<std::string> mat_names;  // material index -> name
  std::vector<std::string> mat_tex;    // material index -> texture PNG path
  // Per triangle: 3 corners of (position index, uv index), plus material.
  std::vector<uint32_t> tri_v;
  std::vector<int> tri_t;
  std::vector<int> tri_mat;
  int current_mat = -1;

  std::string line;
  while (std::getline(obj, line)) {
    if (line.size() < 2) {
      continue;
    }
    if (line[0] == 'v' && line[1] == ' ') {
      char* end = nullptr;
      const float x = std::strtof(line.c_str() + 2, &end);
      const float y = std::strtof(end, &end);
      const float z = std::strtof(end, &end);
      pos.push_back(x);
      pos.push_back(y);
      pos.push_back(z);
    } else if (line[0] == 'v' && line[1] == 't') {
      char* end = nullptr;
      const float u = std::strtof(line.c_str() + 2, &end);
      const float v = std::strtof(end, &end);
      uvs.push_back(u);
      uvs.push_back(v);
    } else if (line.rfind("mtllib", 0) == 0) {
      std::string mtl = TrimCopy(line.substr(6));
      std::ifstream mf(dir + mtl);
      std::string ml;
      int cur = -1;
      while (std::getline(mf, ml)) {
        if (ml.rfind("newmtl", 0) == 0) {
          mat_names.push_back(TrimCopy(ml.substr(6)));
          mat_tex.emplace_back();
          cur = static_cast<int>(mat_names.size()) - 1;
        } else if (cur >= 0) {
          const size_t kp = ml.find("map_Kd");
          if (kp != std::string::npos) {
            std::string tf = TrimCopy(ml.substr(kp + 6));
            if (!tf.empty()) {
              mat_tex[static_cast<size_t>(cur)] = dir + tf;
            }
          }
        }
      }
    } else if (line.rfind("usemtl", 0) == 0) {
      std::string nm = TrimCopy(line.substr(6));
      current_mat = -1;
      for (int i = 0; i < static_cast<int>(mat_names.size()); ++i) {
        if (mat_names[i] == nm) {
          current_mat = i;
          break;
        }
      }
    } else if (line[0] == 'f' && line[1] == ' ') {
      int fv[32];
      int ft[32];
      int n = 0;
      const char* p = line.c_str() + 1;
      const int total = static_cast<int>(pos.size() / 3);
      const int totuv = static_cast<int>(uvs.size() / 2);
      while (n < 32) {
        while (*p == ' ') {
          ++p;
        }
        if (!*p) {
          break;
        }
        char* end = nullptr;
        long vi = std::strtol(p, &end, 10);  // first int of "v" / "v/vt" / "v/vt/vn"
        if (end == p) {
          break;
        }
        if (vi < 0) {
          vi = total + vi + 1;
        }
        long ti = 0;
        p = end;
        if (*p == '/') {
          ++p;
          if (*p != '/') {  // has a vt index
            char* e2 = nullptr;
            ti = std::strtol(p, &e2, 10);
            if (ti < 0) {
              ti = totuv + ti + 1;
            }
            p = e2;
          }
          if (*p == '/') {  // has a vn index (skipped; normals recomputed)
            ++p;
            char* e3 = nullptr;
            (void)std::strtol(p, &e3, 10);
            p = e3;
          }
        }
        fv[n] = static_cast<int>(vi);
        ft[n] = static_cast<int>(ti);
        ++n;
        while (*p && *p != ' ') {
          ++p;
        }
      }
      for (int k = 1; k + 1 < n; ++k) {  // triangulate as a fan
        if (fv[0] >= 1 && fv[k] >= 1 && fv[k + 1] >= 1 && fv[0] <= total &&
            fv[k] <= total && fv[k + 1] <= total) {
          tri_v.push_back(static_cast<uint32_t>(fv[0] - 1));
          tri_v.push_back(static_cast<uint32_t>(fv[k] - 1));
          tri_v.push_back(static_cast<uint32_t>(fv[k + 1] - 1));
          tri_t.push_back(ft[0]);
          tri_t.push_back(ft[k]);
          tri_t.push_back(ft[k + 1]);
          tri_mat.push_back(current_mat < 0 ? 0 : current_mat);
        }
      }
    }
  }

  if (pos.empty() || tri_mat.empty()) {
    WarnOnce("custom avatar '" + name + "' parsed to an empty mesh");
    return;
  }
  const size_t nverts = pos.size() / 3;
  const size_t ntris = tri_mat.size();

  // Smooth per-position normals from face geometry (area-weighted).
  std::vector<float> nacc(nverts * 3, 0.0f);
  for (size_t t = 0; t < ntris; ++t) {
    const uint32_t a = tri_v[t * 3], b = tri_v[t * 3 + 1], c = tri_v[t * 3 + 2];
    const float* pa = &pos[a * 3];
    const float* pb = &pos[b * 3];
    const float* pc = &pos[c * 3];
    const float e1x = pb[0] - pa[0], e1y = pb[1] - pa[1], e1z = pb[2] - pa[2];
    const float e2x = pc[0] - pa[0], e2y = pc[1] - pa[1], e2z = pc[2] - pa[2];
    const float nx = e1y * e2z - e1z * e2y;
    const float ny = e1z * e2x - e1x * e2z;
    const float nz = e1x * e2y - e1y * e2x;
    for (uint32_t vi : {a, b, c}) {
      nacc[vi * 3] += nx;
      nacc[vi * 3 + 1] += ny;
      nacc[vi * 3 + 2] += nz;
    }
  }
  g_state.normals.assign(nverts * 3, 0.0f);
  for (size_t v = 0; v < nverts; ++v) {
    float nx = nacc[v * 3], ny = nacc[v * 3 + 1], nz = nacc[v * 3 + 2];
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-12f) {
      nx /= len;
      ny /= len;
      nz /= len;
    } else {
      nx = 0.0f;
      ny = 1.0f;
      nz = 0.0f;
    }
    g_state.normals[v * 3] = nx;
    g_state.normals[v * 3 + 1] = ny;
    g_state.normals[v * 3 + 2] = nz;
  }
  // Flips all normals outward when the OBJ winding makes them point inward
  // (negative average dot(normal, vertex - centroid)), which would darken the
  // whole mesh under diffuse lighting.
  {
    double cx = 0.0, cy = 0.0, cz = 0.0;
    for (size_t v = 0; v < nverts; ++v) {
      cx += pos[v * 3];
      cy += pos[v * 3 + 1];
      cz += pos[v * 3 + 2];
    }
    cx /= static_cast<double>(nverts);
    cy /= static_cast<double>(nverts);
    cz /= static_cast<double>(nverts);
    double outward = 0.0;
    for (size_t v = 0; v < nverts; ++v) {
      outward += g_state.normals[v * 3] * (pos[v * 3] - cx) +
                 g_state.normals[v * 3 + 1] * (pos[v * 3 + 1] - cy) +
                 g_state.normals[v * 3 + 2] * (pos[v * 3 + 2] - cz);
    }
    if (outward < 0.0) {
      for (float& n : g_state.normals) {
        n = -n;
      }
      REXKRNL_INFO(
          "[customavatar] computed normals pointed inward (avg outward dot {}); "
          "flipped all normals",
          outward / static_cast<double>(nverts));
    }
  }

  // Textures: slot 0 = shared white fallback; then one slot per distinct
  // sidecar path that loads.
  g_state.textures.clear();
  {
    RgbaImage white;
    white.width = 128;
    white.height = 128;
    white.rgba.assign(static_cast<size_t>(128) * 128 * 4, 0xFF);
    g_state.textures.push_back(std::move(white));
  }
  const int nmats = static_cast<int>(mat_names.size());
  std::vector<int> mat_slot(std::max(nmats, 1), 0);
  {
    std::map<std::string, int> path_slot;
    for (int m = 0; m < nmats; ++m) {
      const std::string& path = mat_tex[static_cast<size_t>(m)];
      if (path.empty()) {
        continue;
      }
      auto it = path_slot.find(path);
      if (it != path_slot.end()) {
        mat_slot[static_cast<size_t>(m)] = it->second;
        continue;
      }
      RgbaImage img;
      if (LoadRgbaSidecar(path, img) &&
          g_state.textures.size() < 18) {  // stock model texture-array cap
        NormalizeImage(img);
        mat_slot[static_cast<size_t>(m)] = static_cast<int>(g_state.textures.size());
        path_slot[path] = mat_slot[static_cast<size_t>(m)];
        g_state.textures.push_back(std::move(img));
      } else {
        REXKRNL_INFO("[customavatar] no .rgba sidecar for '{}' (using white)", path);
        path_slot[path] = 0;
      }
    }
  }

  // One batch per material that owns triangles, capped at 16 (the stock batch
  // array limit) with overflow folded into the last batch.
  const int kMaxBatches = 16;
  std::vector<int> mat_batch(std::max(nmats, 1) + 1, -1);  // +1: "no material" bucket
  g_state.batches.clear();
  // Per-batch (position index, half u, half v) -> output vertex dedup.
  std::vector<std::map<uint64_t, uint16_t>> dedup;
  auto batch_index_for = [&](int mat) -> size_t {
    int mi = (mat >= 0 && mat < nmats) ? mat : nmats;
    if (mat_batch[static_cast<size_t>(mi)] < 0) {
      if (static_cast<int>(g_state.batches.size()) >= kMaxBatches) {
        mat_batch[static_cast<size_t>(mi)] = kMaxBatches - 1;
      } else {
        mat_batch[static_cast<size_t>(mi)] = static_cast<int>(g_state.batches.size());
        g_state.batches.emplace_back();
        g_state.batches.back().texture_slot =
            (mi < nmats) ? mat_slot[static_cast<size_t>(mi)] : 0;
        dedup.emplace_back();
      }
    }
    return static_cast<size_t>(mat_batch[static_cast<size_t>(mi)]);
  };
  auto corner = [&](CustomBatch& batch, std::map<uint64_t, uint16_t>& seen,
                    uint32_t vi, int ti) -> int {
    float u = 0.0f, v = 0.0f;
    if (ti >= 1 && static_cast<size_t>(ti) * 2 <= uvs.size()) {
      u = uvs[(static_cast<size_t>(ti) - 1) * 2];
      // OBJ vt uses a bottom-left origin and guest half UVs a top-left one, so
      // v is inverted here.
      v = 1.0f - uvs[(static_cast<size_t>(ti) - 1) * 2 + 1];
    }
    const uint16_t hu = rex::float_to_xenos_half(u);
    const uint16_t hv = rex::float_to_xenos_half(v);
    const uint64_t key = (static_cast<uint64_t>(vi) << 32) |
                         (static_cast<uint64_t>(hu) << 16) | hv;
    auto it = seen.find(key);
    if (it != seen.end()) {
      return it->second;
    }
    if (batch.src_vertex.size() >= 0xFFFFu) {
      return -1;  // u16 index space exhausted; drop the triangle
    }
    const uint16_t idx = static_cast<uint16_t>(batch.src_vertex.size());
    batch.src_vertex.push_back(vi);
    Vector2<uint16_t> huv;
    huv.x = hu;
    huv.y = hv;
    batch.uvs.push_back(huv);
    seen[key] = idx;
    return idx;
  };
  for (size_t t = 0; t < ntris; ++t) {
    const size_t bidx = batch_index_for(tri_mat[t]);
    CustomBatch& batch = g_state.batches[bidx];
    int i0 = corner(batch, dedup[bidx], tri_v[t * 3], tri_t[t * 3]);
    int i1 = corner(batch, dedup[bidx], tri_v[t * 3 + 1], tri_t[t * 3 + 1]);
    int i2 = corner(batch, dedup[bidx], tri_v[t * 3 + 2], tri_t[t * 3 + 2]);
    if (i0 < 0 || i1 < 0 || i2 < 0) {
      continue;
    }
    batch.indices.push_back(static_cast<uint16_t>(i0));
    batch.indices.push_back(static_cast<uint16_t>(i1));
    batch.indices.push_back(static_cast<uint16_t>(i2));
  }
  // Drop batches that ended up with no triangles.
  g_state.batches.erase(
      std::remove_if(g_state.batches.begin(), g_state.batches.end(),
                     [](const CustomBatch& b) { return b.indices.empty(); }),
      g_state.batches.end());
  if (g_state.batches.empty()) {
    WarnOnce("custom avatar '" + name + "' has no renderable triangles");
    return;
  }

  g_state.pos.swap(pos);
  g_state.from_baked = used_baked;
  g_state.loaded = true;

  size_t total_out_verts = 0, total_out_tris = 0;
  for (const auto& b : g_state.batches) {
    total_out_verts += b.src_vertex.size();
    total_out_tris += b.indices.size() / 3;
  }
  REXKRNL_INFO(
      "[customavatar] loaded '{}' for native substitution: {} src verts -> {} out "
      "verts, {} tris, {} batches, {} textures, baked={}",
      name, g_state.pos.size() / 3, total_out_verts, total_out_tris,
      g_state.batches.size(), g_state.textures.size() - 1, used_baked ? 1 : 0);
}

void EnsureLoadedLocked() {
  if (!g_state.tried) {
    LoadCustomAvatarLocked();
  }
}

// --- guest model construction -------------------------------------------------

// Computes the cpu/gpu buffer footprint of the custom model for a given
// per-vertex size, mirroring BuildCustomModel's layout.
void ComputeModelSizes(uint32_t vertex_size, uint32_t* cpu_size,
                       uint32_t* gpu_size) {
  uint32_t gpu = 0;
  for (const auto& b : g_state.batches) {
    gpu += static_cast<uint32_t>(b.src_vertex.size()) * vertex_size;
    gpu = rex::align(gpu, 16u);
  }
  for (const auto& b : g_state.batches) {
    gpu += static_cast<uint32_t>(b.indices.size()) * 2u;
    gpu = rex::align(gpu, 16u);
  }
  for (const auto& t : g_state.textures) {
    gpu = rex::align(gpu, 4096u);
    gpu += rex::align(t.width * 4u, 512u) * t.height;
  }
  gpu = rex::align(gpu, 4096u);
  uint32_t cpu = static_cast<uint32_t>(g_state.batches.size()) * 512u +
                 static_cast<uint32_t>(g_state.textures.size()) * 44u;
  cpu = rex::align(cpu, 16u);
  *cpu_size = cpu;
  *gpu_size = gpu;
}

// Builds the substitute host Model from the cached custom mesh, laying the
// vertex data out in the replaced component's original layout (vertex size /
// uv count / index size / shader id+parameters cloned from template_batch).
std::shared_ptr<Model> BuildCustomModel(const TriangleBatch& template_batch) {
  uint32_t uv_count = std::min(std::max(template_batch.uv_count, 1u), 6u);
  uint32_t vertex_size = 32u + 4u * (uv_count - 1u);
  const bool flip = REXCVAR_GET(dcc2_custom_avatar_flip_winding);

  auto model = std::make_shared<Model>();

  // GPU layout: all vertex arrays, then all index arrays, then textures.
  uint32_t off = 0;
  std::vector<uint32_t> vert_offsets, idx_offsets;
  for (const auto& b : g_state.batches) {
    vert_offsets.push_back(off);
    off += static_cast<uint32_t>(b.src_vertex.size()) * vertex_size;
    off = rex::align(off, 16u);
  }
  const uint32_t index_buffer_offset = off;
  for (const auto& b : g_state.batches) {
    idx_offsets.push_back(off);
    off += static_cast<uint32_t>(b.indices.size()) * 2u;
    off = rex::align(off, 16u);
  }
  const uint32_t index_buffer_end = off;
  std::vector<uint32_t> tex_offsets, tex_sizes;
  for (const auto& t : g_state.textures) {
    off = rex::align(off, 4096u);
    tex_offsets.push_back(off);
    const uint32_t bytes = rex::align(t.width * 4u, 512u) * t.height;
    tex_sizes.push_back(bytes);
    off += bytes;
  }
  const uint32_t gpu_size = rex::align(off, 4096u);

  model->gpu_size = gpu_size;
  model->vertex_buffer_offset = 0;
  model->vertex_buffer_size = index_buffer_offset;
  model->index_buffer_offset = index_buffer_offset;
  model->index_buffer_size = index_buffer_end - index_buffer_offset;
  model->texture_buffer_size = tex_offsets.empty()
                                   ? 0
                                   : (gpu_size - tex_offsets.front());
  model->triangle_batch_array_offset = 0;
  model->texture_array_offset =
      static_cast<uint32_t>(g_state.batches.size()) * 512u;
  model->cpu_size = rex::align(
      static_cast<uint32_t>(g_state.batches.size()) * 512u +
          static_cast<uint32_t>(g_state.textures.size()) * 44u,
      16u);
  model->texture_scratch_size = 0;

  for (size_t bi = 0; bi < g_state.batches.size(); ++bi) {
    const CustomBatch& src = g_state.batches[bi];
    TriangleBatch batch{};
    batch.shader_id = template_batch.shader_id;
    batch.shader_parameters = template_batch.shader_parameters;
    // Every texture parameter is redirected to this batch's texture and
    // samples UV set 0, since all uv sets carry the same coordinates.
    bool has_texture_param = false;
    for (auto& param : batch.shader_parameters) {
      if (param.type == ShaderParameterType::kTexture) {
        param.texture.index = static_cast<uint16_t>(src.texture_slot);
        param.texture.uv_layer = 0;
        has_texture_param = true;
      }
    }
    if (!has_texture_param) {
      REXKRNL_INFO(
          "[customavatar] template batch has no texture parameter; batch {} will "
          "render untextured",
          bi);
    }
    batch.triangle_count = static_cast<uint32_t>(src.indices.size() / 3);
    batch.uv_count = uv_count;
    batch.vertex_size = vertex_size;
    batch.index_size = 2;
    batch.vertex_array_offset = vert_offsets[bi];
    batch.index_array_offset = idx_offsets[bi];

    // Flipping the winding flips which side is front-facing, so the shading
    // normals flip with it.
    const float nsign = flip ? -1.0f : 1.0f;
    const bool debug_normals = REXCVAR_GET(dcc2_custom_avatar_debug_normals);
    const uint32_t up_normal = PackNormal(0.0f, 1.0f, 0.0f);
    batch.vertices.reserve(src.src_vertex.size());
    for (size_t v = 0; v < src.src_vertex.size(); ++v) {
      const uint32_t sv = src.src_vertex[v];
      Vertex vert{};
      vert.position.x = g_state.pos[sv * 3];
      vert.position.y = g_state.pos[sv * 3 + 1];
      vert.position.z = g_state.pos[sv * 3 + 2];
      vert.normal = debug_normals
                        ? up_normal
                        : PackNormal(nsign * g_state.normals[sv * 3],
                                     nsign * g_state.normals[sv * 3 + 1],
                                     nsign * g_state.normals[sv * 3 + 2]);
      vert.blend_weight = g_state.blend_weight[sv];
      vert.blend_indices = g_state.blend_indices[sv];
      vert.color = 0xFFFFFFFFu;
      for (uint32_t u = 0; u < uv_count; ++u) {
        vert.uvs.push_back(src.uvs[v]);  // duplicate UV0 into every uv set
      }
      batch.vertices.push_back(std::move(vert));
    }

    batch.indices.reserve(src.indices.size());
    for (size_t t = 0; t + 2 < src.indices.size(); t += 3) {
      if (flip) {
        batch.indices.push_back(src.indices[t]);
        batch.indices.push_back(src.indices[t + 2]);
        batch.indices.push_back(src.indices[t + 1]);
      } else {
        batch.indices.push_back(src.indices[t]);
        batch.indices.push_back(src.indices[t + 1]);
        batch.indices.push_back(src.indices[t + 2]);
      }
    }

    model->triangle_batches.push_back(std::move(batch));
  }

  for (size_t ti = 0; ti < g_state.textures.size(); ++ti) {
    const RgbaImage& img = g_state.textures[ti];
    ModelTexture mt{};
    mt.gpu_offset = tex_offsets[ti];
    mt.gpu_size = tex_sizes[ti];
    // D3DFMT_LIN_A8R8G8B8 with ENDIAN_8IN16 (matches the stock avatar packs'
    // 16-bit-swapped storage convention).
    mt.texture.format = 0x18280046u;
    mt.texture.width = img.width;
    mt.texture.height = img.height;
    mt.texture.layer_count = 1;
    mt.texture.is_empty = false;
    mt.texture.is_tiled = false;
    mt.texture.data_stride = img.width * 4u;
    mt.texture.data_rows = img.height;
    mt.texture.data_size = tex_sizes[ti];
    mt.texture.total_data_size = tex_sizes[ti];
    // RGBA -> little-endian ARGB dword bytes [B,G,R,A] -> 16-bit byte swap
    // => [G,B,A,R] per texel (what the guest/GPU convention expects).
    mt.texture.data_bytes.resize(img.rgba.size());
    for (size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
      const uint8_t* s = &img.rgba[i];
      uint8_t* p = &mt.texture.data_bytes[i];
      p[0] = s[1];  // G
      p[1] = s[2];  // B
      p[2] = s[3];  // A
      p[3] = s[0];  // R
    }
    model->textures.push_back(std::move(mt));
  }

  return model;
}

// --- weight transfer (mirrors sentient.cpp's "transferred" block) ------------

// Collects (position, packed weights, packed indices) of every vertex of the
// decoded default avatar, auto-fits the custom mesh into its bounding box when
// the mesh is not the editor's pre-aligned bake, then assigns each custom
// vertex the weights/indices of the nearest default vertex.
bool TransferWeightsLocked(
    const std::vector<std::pair<X_AVATAR_COMPONENT_INFO, std::shared_ptr<Model>>>&
        source_components) {
  if (g_state.transferred) {
    return true;
  }
  std::vector<float> opos;
  std::vector<uint32_t> obw, obi;
  for (const auto& component : source_components) {
    if (!component.second) {
      continue;
    }
    for (const auto& batch : component.second->triangle_batches) {
      for (const auto& v : batch.vertices) {
        opos.push_back(v.position.x);
        opos.push_back(v.position.y);
        opos.push_back(v.position.z);
        obw.push_back(v.blend_weight);
        obi.push_back(v.blend_indices);
      }
    }
  }
  const size_t on = obw.size();
  if (!on) {
    return false;
  }
  const size_t cnv = g_state.pos.size() / 3;

  // Auto-fits the custom mesh into the default mesh's bounding box (uniform
  // scale by height plus recenter), skipped for model_baked.obj, which the
  // editor already aligned to the skeleton.
  if (!g_state.from_baked && !g_state.fitted && cnv > 0) {
    float o_min[3] = {1e30f, 1e30f, 1e30f}, o_max[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t o = 0; o < on; ++o) {
      for (int k = 0; k < 3; ++k) {
        const float val = opos[o * 3 + k];
        o_min[k] = std::min(o_min[k], val);
        o_max[k] = std::max(o_max[k], val);
      }
    }
    float c_min[3] = {1e30f, 1e30f, 1e30f}, c_max[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t v = 0; v < cnv; ++v) {
      for (int k = 0; k < 3; ++k) {
        const float val = g_state.pos[v * 3 + k];
        c_min[k] = std::min(c_min[k], val);
        c_max[k] = std::max(c_max[k], val);
      }
    }
    const float oh = o_max[1] - o_min[1], ch = c_max[1] - c_min[1];
    const float fit = (ch > 1e-6f) ? (oh / ch) : 1.0f;
    float oc[3], cc[3];
    for (int k = 0; k < 3; ++k) {
      oc[k] = (o_min[k] + o_max[k]) * 0.5f;
      cc[k] = (c_min[k] + c_max[k]) * 0.5f;
    }
    for (size_t v = 0; v < cnv; ++v) {
      for (int k = 0; k < 3; ++k) {
        g_state.pos[v * 3 + k] = (g_state.pos[v * 3 + k] - cc[k]) * fit + oc[k];
      }
    }
    REXKRNL_INFO("[customavatar] auto-fit scale={}", fit);
  }
  g_state.fitted = true;

  g_state.blend_weight.assign(cnv, 0u);
  g_state.blend_indices.assign(cnv, 0u);
  for (size_t v = 0; v < cnv; ++v) {
    const float bx = g_state.pos[v * 3], by = g_state.pos[v * 3 + 1],
                bz = g_state.pos[v * 3 + 2];
    float best = 1e30f;
    size_t best_o = 0;
    for (size_t o = 0; o < on; ++o) {
      const float dx = bx - opos[o * 3];
      const float dy = by - opos[o * 3 + 1];
      const float dz = bz - opos[o * 3 + 2];
      const float d = dx * dx + dy * dy + dz * dz;
      if (d < best) {
        best = d;
        best_o = o;
      }
    }
    g_state.blend_weight[v] = obw[best_o];
    g_state.blend_indices[v] = obi[best_o];
  }
  g_state.transferred = true;
  REXKRNL_INFO("[customavatar] transferred weights to {} verts from {} default verts",
               cnv, on);
  return true;
}

}  // namespace

// --- public API ----------------------------------------------------------------

bool CustomAvatarActive() {
  if (!REXCVAR_GET(dcc2_custom_avatar_native)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureLoadedLocked();
  return g_state.loaded;
}

void GetCustomAvatarExtraSizes(uint32_t* extra_cpu_size, uint32_t* extra_gpu_size) {
  *extra_cpu_size = 0;
  *extra_gpu_size = 0;
  if (!REXCVAR_GET(dcc2_custom_avatar_native)) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureLoadedLocked();
  if (!g_state.loaded) {
    return;
  }
  // Worst-case vertex layout (uv_count 6 => 52 bytes/vertex) plus slack, an
  // upper bound on the model built later.
  uint32_t cpu = 0, gpu = 0;
  ComputeModelSizes(52u, &cpu, &gpu);
  *extra_cpu_size = rex::align(cpu + 4096u, 16u);
  *extra_gpu_size = rex::align(gpu + 65536u, 4096u);
}

// Per-thread substitution suppression for AI/NPC avatar builds, thread-local
// because XamAvatarGetAssets runs concurrently on several guest threads during
// a level load.
static thread_local bool tls_substitution_suppressed = false;
// Per-thread queued STRB special asset (DCC2 ai-assets look) for the next
// build, consumed by AppendPendingExtraComponent.
static thread_local std::vector<uint8_t> tls_pending_extra_strb;

void SetCustomAvatarSuppressed(bool suppressed) {
  tls_substitution_suppressed = suppressed;
}

void SetPendingExtraComponentStrb(const uint8_t* data, size_t size) {
  if (!data || !size) {
    tls_pending_extra_strb.clear();
    return;
  }
  tls_pending_extra_strb.assign(data, data + size);
}

int AppendPendingExtraComponent(
    std::vector<std::pair<X_AVATAR_COMPONENT_INFO, std::shared_ptr<Model>>>&
        source_components,
    ModelLoadOptions model_load_options) {
  if (tls_pending_extra_strb.empty()) {
    return -1;
  }
  const std::vector<uint8_t> strb = std::move(tls_pending_extra_strb);
  tls_pending_extra_strb.clear();
  auto model = Model::Load(strb.data(), strb.size(), model_load_options);
  if (!model) {
    REXKRNL_WARN("[customavatar] queued extra STRB component failed to decode ({} bytes)",
                 strb.size());
    return -1;
  }
  // Zero asset id and kNone categories give the guest an extra component with
  // no identity, and the serializer keys the neutral shader overrides off the
  // returned index.
  X_AVATAR_COMPONENT_INFO info{};
  source_components.push_back({info, model});
  REXKRNL_INFO("[customavatar] appended extra STRB component ({} bytes -> component {})",
               strb.size(), source_components.size() - 1);
  return static_cast<int>(source_components.size() - 1);
}

int ApplyCustomAvatar(
    std::vector<std::pair<X_AVATAR_COMPONENT_INFO, std::shared_ptr<Model>>>&
        source_components) {
  if (tls_substitution_suppressed) {
    return -1;  // AI/NPC build: keep the decoded stock avatar
  }
  if (!REXCVAR_GET(dcc2_custom_avatar_native) || source_components.empty()) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureLoadedLocked();
  if (!g_state.loaded) {
    return -1;
  }

  // The component with the most vertices (typically the body) is the
  // substitution target.
  size_t target = SIZE_MAX;
  size_t target_verts = 0;
  for (size_t i = 0; i < source_components.size(); ++i) {
    const auto& model = source_components[i].second;
    if (!model || model->triangle_batches.empty()) {
      continue;
    }
    size_t verts = 0;
    for (const auto& batch : model->triangle_batches) {
      verts += batch.vertices.size();
    }
    if (target == SIZE_MAX || verts > target_verts) {
      target = i;
      target_verts = verts;
    }
  }
  if (target == SIZE_MAX) {
    WarnOnce("no decoded default component to substitute into");
    return -1;
  }

  if (!TransferWeightsLocked(source_components)) {
    WarnOnce("default avatar has no vertices for weight transfer");
    return -1;
  }

  // The shader/layout template prefers a clothing component's batch over the
  // body's, because the body batch is the skin material whose shader path
  // multiplies the texture by the profile skin tone (usage 22 ->
  // CustomColor/SkinTone constants).
  const TriangleBatch* template_batch =
      &source_components[target].second->triangle_batches[0];
  {
    static const uint32_t kTemplatePreference[] = {
        ComponentCategory::kTop,    ComponentCategory::kBottom,
        ComponentCategory::kShoes,  ComponentCategory::kGloves,
        ComponentCategory::kHat,    ComponentCategory::kGlasses,
        ComponentCategory::kWristwear,
    };
    bool found = false;
    for (uint32_t want : kTemplatePreference) {
      for (const auto& component : source_components) {
        if (!component.second || component.second->triangle_batches.empty()) {
          continue;
        }
        if (static_cast<uint32_t>(component.first.categories) & want) {
          template_batch = &component.second->triangle_batches[0];
          REXKRNL_INFO(
              "[customavatar] shader template from clothing component "
              "(categories={:#x}, shader_id={:#x})",
              static_cast<uint32_t>(component.first.categories),
              template_batch->shader_id);
          found = true;
          break;
        }
      }
      if (found) {
        break;
      }
    }
    if (!found) {
      REXKRNL_INFO(
          "[customavatar] no clothing component found; shader template from "
          "substituted component (shader_id={:#x})",
          template_batch->shader_id);
    }
  }

  std::shared_ptr<Model> custom_model;
  try {
    custom_model = BuildCustomModel(*template_batch);
  } catch (const std::exception& e) {
    WarnOnce(std::string("failed to build substitute model: ") + e.what());
    return -1;
  }
  if (!custom_model || custom_model->triangle_batches.empty()) {
    WarnOnce("substitute model construction produced no batches");
    return -1;
  }

  // Swaps in the custom model, keeping the component info (category bits).
  source_components[target].second = custom_model;

  // Every other component is reduced to zero triangles, so its structs, batch
  // arrays, textures and buffer claims stay valid and draw nothing.
  for (size_t i = 0; i < source_components.size(); ++i) {
    if (i == target || !source_components[i].second) {
      continue;
    }
    for (auto& batch : source_components[i].second->triangle_batches) {
      batch.triangle_count = 0;
      batch.vertices.clear();
      batch.indices.clear();
    }
  }

  REXKRNL_INFO(
      "[customavatar] native substitution: '{}' replaced component {} "
      "(categories={:#x}, {} default verts), {} batches, cpu={:#x} gpu={:#x}",
      g_state.name, target,
      static_cast<uint32_t>(source_components[target].first.categories),
      target_verts, custom_model->triangle_batches.size(),
      static_cast<uint32_t>(custom_model->cpu_size),
      static_cast<uint32_t>(custom_model->gpu_size));
  return static_cast<int>(target);
}

}  // namespace avatars
}  // namespace rex
