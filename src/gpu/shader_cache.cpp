// GPL-3.0 — see LICENSE in this directory.

#include "shader_cache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>

#include <rex/logging.h>

namespace rex::videonative {

namespace {

// One file of the pack by name; false when the pack has none.
using PackReader = std::function<bool(const std::string&, std::vector<uint8_t>*)>;

bool ReadWhole(const std::filesystem::path& path, std::vector<uint8_t>* out) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) return false;
  out->resize(size_t(file.tellg()));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(out->data()), out->size());
  return true;
}

PackReader FromDirectory(const std::filesystem::path& dir) {
  return [dir](const std::string& name, std::vector<uint8_t>* out) {
    return ReadWhole(dir / name, out);
  };
}

// Single-file pack reader (little-endian "CC1P", u32 version 1, u32 count, then
// per file a u16 name length, the name and a u32 size, then the file bodies in
// that order), null for a file that is not one.
PackReader FromFile(const std::filesystem::path& path) {
  auto bytes = std::make_shared<std::vector<uint8_t>>();
  auto files = std::make_shared<
      std::unordered_map<std::string, std::pair<size_t, size_t>>>();
  if (!ReadWhole(path, bytes.get())) return nullptr;
  const std::vector<uint8_t>& b = *bytes;
  size_t at = 0;
  auto take = [&](size_t n, uint32_t* value) {
    if (at + n > b.size()) return false;
    *value = 0;
    memcpy(value, b.data() + at, n);
    at += n;
    return true;
  };
  uint32_t magic = 0, version = 0, count = 0;
  if (!take(4, &magic) || memcmp(&magic, "CC1P", 4) != 0 ||
      !take(4, &version) || version != 1 || !take(4, &count)) {
    return nullptr;
  }
  std::vector<std::pair<std::string, uint32_t>> table;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t length = 0, size = 0;
    if (!take(2, &length) || at + length > b.size()) return nullptr;
    std::string name(reinterpret_cast<const char*>(b.data() + at), length);
    at += length;
    if (!take(4, &size)) return nullptr;
    table.emplace_back(std::move(name), size);
  }
  for (auto& [name, size] : table) {
    if (at + size > b.size()) return nullptr;
    files->emplace(std::move(name), std::make_pair(at, size_t(size)));
    at += size;
  }
  return [bytes, files](const std::string& name, std::vector<uint8_t>* out) {
    auto it = files->find(name);
    if (it == files->end()) return false;
    out->assign(bytes->begin() + it->second.first,
                bytes->begin() + it->second.first + it->second.second);
    return true;
  };
}

bool ReadRow(const PackReader& read, std::string line, uint64_t* ucode_hash,
             bool* pixel, PackShader* out);

}  // namespace

bool ShaderCache::LoadRow(const std::filesystem::path& pack_dir,
                          std::string line, uint64_t* ucode_hash, bool* pixel,
                          PackShader* out) {
  return ReadRow(FromDirectory(pack_dir), std::move(line), ucode_hash, pixel,
                 out);
}

namespace {

// Manifest columns:
// runtime_ucode_hash,type,fingerprint,dxil,bindings,float_bitmap,reason.
bool ReadRow(const PackReader& read, std::string line, uint64_t* ucode_hash,
             bool* pixel, PackShader* out) {
  if (!line.empty() && line.back() == '\r') line.pop_back();
  std::string columns[5];
  size_t column_start = 0;
  for (uint32_t i = 0; i < 5 && column_start <= line.size(); ++i) {
    size_t comma = line.find(',', column_start);
    columns[i] = line.substr(
        column_start,
        (comma == std::string::npos ? line.size() : comma) - column_start);
    column_start = (comma == std::string::npos) ? line.size() + 1 : comma + 1;
  }
  if (columns[3].empty()) return false;  // not generated (translator fallback)

  PackShader& entry = *out;
  if (!read(columns[3], &entry.dxil)) {
    REXGPU_WARN("videonative: shader pack missing {}", columns[3]);
    return false;
  }

  // Trimmed VS variant ({hash}.vst.dxil) with unwritten outputs removed from
  // the signature, absent in packs without it.
  if (columns[1] == "vs") read(columns[0] + ".vst.dxil", &entry.dxil_trim);

  // "kind:fetch_constant:dimension:is_signed" per b4 slot, ';'-separated.
  const std::string& bindings = columns[4];
  size_t binding_start = 0;
  bool bindings_valid = true;
  while (binding_start < bindings.size()) {
    size_t separator = bindings.find(';', binding_start);
    size_t binding_end =
        (separator == std::string::npos) ? bindings.size() : separator;
    PackShader::Binding binding = {};
    uint32_t fields[3] = {};
    if (binding_end - binding_start >= 2 &&
        (bindings[binding_start] == 's' || bindings[binding_start] == 't') &&
        bindings[binding_start + 1] == ':' &&
        sscanf(bindings.c_str() + binding_start + 2, "%u:%u:%u", &fields[0],
               &fields[1], &fields[2]) == 3) {
      binding.is_sampler = bindings[binding_start] == 's' ? 1 : 0;
      binding.fetch_constant = uint8_t(fields[0]);
      binding.dimension = uint8_t(fields[1]);
      binding.is_signed = uint8_t(fields[2]);
      entry.bindings.push_back(binding);
    } else {
      bindings_valid = false;
      break;
    }
    binding_start =
        (separator == std::string::npos) ? bindings.size() : separator + 1;
  }
  if (!bindings_valid) {
    REXGPU_WARN("videonative: shader pack bad bindings for {}, skipping",
                columns[0]);
    return false;
  }

  *ucode_hash = strtoull(columns[0].c_str(), nullptr, 16);
  *pixel = columns[1] == "ps";
  return true;
}

}  // namespace

bool ShaderCache::Load(const std::string& pack_str) {
  const std::filesystem::path pack(pack_str);
  std::error_code ec;
  const PackReader read = std::filesystem::is_regular_file(pack, ec)
                              ? FromFile(pack)
                              : FromDirectory(pack);
  std::vector<uint8_t> manifest_bytes;
  if (!read || !read("manifest.csv", &manifest_bytes)) {
    REXGPU_WARN("videonative: shader pack manifest not found in {}", pack_str);
    return false;
  }
  std::istringstream manifest(
      std::string(manifest_bytes.begin(), manifest_bytes.end()));

  std::string line;
  std::getline(manifest, line);  // header
  while (std::getline(manifest, line)) {
    uint64_t ucode_hash = 0;
    bool pixel = false;
    PackShader entry;
    if (!ReadRow(read, line, &ucode_hash, &pixel, &entry)) continue;
    (pixel ? pixel_shaders_ : vertex_shaders_)
        .emplace(ucode_hash, std::move(entry));
  }

  auto load_blob = [&](const char* name, std::vector<uint8_t>* out,
                       const char* missing_consequence) {
    if (!read(name, out)) {
      REXGPU_WARN("videonative: pack has no {} — {} (regenerate the pack)",
                  name, missing_consequence);
    }
  };
  load_blob("rect_expand.gs.dxil", &rect_gs_dxil_,
            "RECTLIST draws will be skipped");
  load_blob("point_expand.gs.dxil", &point_gs_dxil_,
            "POINTLIST draws will render as 1px points");
  load_blob("clip_planes.gs.dxil", &clip_gs_dxil_,
            "user clip planes will not clip geometry");
  load_blob("blit.vs.dxil", &blit_vs_dxil_, "Swap composite will be skipped");
  load_blob("blit.ps.dxil", &blit_ps_dxil_, "Swap composite will be skipped");

  loaded_ = !vertex_shaders_.empty() || !pixel_shaders_.empty();
  REXGPU_INFO("videonative: shader pack loaded — {} vs + {} ps{} from {}",
              vertex_shaders_.size(), pixel_shaders_.size(),
              rect_gs_dxil_.empty() ? "" : " + rect GS", pack_str);
  return loaded_;
}

}  // namespace rex::videonative
