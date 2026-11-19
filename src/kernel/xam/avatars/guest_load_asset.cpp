/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstring>
#include <filesystem>
#include <stack>

#include "guest_load_asset.h"

#include "asset_pack.h"
#include "blend_shape.h"
#include "blend_shape_apply.h"
#include "closet.h"
#include "compression.h"
#include "custom_avatar.h"
#include "guest_animation.h"
#include "guest_asset.h"
#include "guest_load_animation.h"
#include "memory_block.h"
#include "model.h"
#include "model_save.h"
#include "prop.h"
#include "skeleton.h"
#include "skeleton_data.h"
#include "skeleton_scaling.h"
#include "strb.h"
#include "bit_stream.h"
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "xe_compat.h"

namespace rex {
namespace avatars {

// Legacy (pre-Fall-2010) asset pack registered by xam_avatar.cpp, the source
// of the old-revision body meshes for the old-body item rescue, with bodies
// cached per gender after first decode.
static AssetPack* g_legacy_asset_pack = nullptr;
static std::shared_ptr<Model> g_legacy_bodies[2];  // [0] male c=1, [1] female c=2
static ModelLoadOptions g_legacy_body_options[2];

void SetLegacyAssetPack(AssetPack* pack) {
  g_legacy_asset_pack = pack;
  g_legacy_bodies[0].reset();
  g_legacy_bodies[1].reset();
}

// load_options must match the options the worn components were decoded with
// (kInvert per the title's coordinate system), or the patch-to-legacy-body
// comparison reads a ~10cm Z mismatch and the rescue takes the affine
// fallback.
static std::shared_ptr<Model> LoadLegacyBody(uint16_t gender_c,
                                             ModelLoadOptions load_options) {
  if (!g_legacy_asset_pack || (gender_c != 1 && gender_c != 2)) {
    return nullptr;
  }
  auto& slot = g_legacy_bodies[gender_c - 1];
  if (slot && g_legacy_body_options[gender_c - 1] == load_options) {
    return slot;
  }
  slot.reset();
  const auto& infos = g_legacy_asset_pack->asset_infos();
  for (size_t i = 0; i < infos.size(); ++i) {
    if (!(infos[i].categories & ComponentCategory::kBody) ||
        infos[i].bodies != (uint32_t)gender_c) {
      continue;
    }
    const uint8_t* buffer = nullptr;
    size_t size = 0;
    if (g_legacy_asset_pack->GetAssetDataByIndex(i, buffer, size)) {
      slot = Model::Load(buffer, size, load_options);
      if (slot) {
        g_legacy_body_options[gender_c - 1] = load_options;
        REXKRNL_INFO("[avatar] legacy body loaded for rescue (gender c={}, pack idx {}, "
                     "options {:#x})",
                     gender_c, i, load_options);
      }
    }
    break;
  }
  return slot;
}

static BodyType GetBodyType(const X_AVATAR_METADATA& metadata) {
  const AssetId male_body_asset_id = {
      2, 0, 1, {0xC1, 0xC8, 0xF1, 0x09, 0xA1, 0x9C, 0xB2, 0xE0}};
  const AssetId female_body_asset_id = {
      2, 0, 2, {0xC1, 0xC8, 0xF1, 0x09, 0xA1, 0x9C, 0xB2, 0xE0}};
  if (metadata.body_component.asset_id == male_body_asset_id) {
    return BodyType::kMale;
  }
  if (metadata.body_component.asset_id == female_body_asset_id) {
    return BodyType::kFemale;
  }
  return BodyType::kUnknown;
}

static bool LoadFile(std::filesystem::path path, std::vector<uint8_t>& buffer) {
  bool was_loaded = false;
  auto handle = rex::filesystem::OpenFile(path, "rb");
  if (handle != nullptr) {
    fseek(handle, 0, SEEK_END);
    buffer.resize(ftell(handle));
    fseek(handle, 0, SEEK_SET);
    size_t bytes_read = fread(buffer.data(), 1, buffer.size(), handle);
    if (bytes_read == buffer.size()) {
      was_loaded = true;
    }
    fclose(handle);
  }
  return was_loaded;
}

static bool LoadAsset(AssetPack* asset_pack, const AssetId& asset_id,
                      const uint8_t*& buffer, size_t& size,
                      std::vector<uint8_t>& temp) {
  // Imported marketplace items (full product GUIDs) resolve through the
  // closet, and only stock-tail ids are pack indexes.
  if (!IsStockPackId(asset_id)) {
    if (GetCloset().ReadItemBytes(asset_id, temp) && !temp.empty()) {
      buffer = temp.data();
      size = temp.size();
      return true;
    }
    // Only zero-tail links go on to the pack, since any other id would pick an unrelated entry by its .b.
    static const uint8_t kZeroTail[8] = {};
    if (std::memcmp(asset_id.d, kZeroTail, sizeof(kZeroTail)) != 0) {
      if (LoadFile(fmt::format("avatar_blobs\\{}.bin", asset_id.to_string()), temp)) {
        buffer = temp.data();
        size = temp.size();
        return true;
      }
      REXKRNL_WARN("[avatar] item {} is in neither the closet nor the cache; the manifest fallback is used",
                   asset_id.to_string());
      return false;
    }
  }
  if (!asset_pack->GetAssetData(asset_id, buffer, size)) {
    // TODO: load from the user data path.
    std::filesystem::path bin_path =
        fmt::format("avatar_blobs\\{}.bin", asset_id.to_string());
    if (!LoadFile(bin_path, temp)) {
      return false;
    }
    buffer = temp.data();
    size = temp.size();
  }
  return true;
}

template <typename Type>
static std::shared_ptr<Type> LoadAsset(AssetPack* asset_pack,
                                       const AssetId& asset_id) {
  const uint8_t* strb_buffer;
  size_t strb_size;
  std::vector<uint8_t> strb_bytes;
  if (!LoadAsset(asset_pack, asset_id, strb_buffer, strb_size, strb_bytes)) {
    return nullptr;
  }
  return Type::Load(strb_buffer, strb_size);
}

template <typename Type, typename Options>
static std::shared_ptr<Type> LoadAsset(AssetPack* asset_pack,
                                       const AssetId& asset_id,
                                       Options load_options) {
  const uint8_t* strb_buffer;
  size_t strb_size;
  std::vector<uint8_t> strb_bytes;
  if (!LoadAsset(asset_pack, asset_id, strb_buffer, strb_size, strb_bytes)) {
    return nullptr;
  }
  return Type::Load(strb_buffer, strb_size, load_options);
}

REXCVAR_DEFINE_BOOL(avatar_eyeshadow_debug, false, "Kernel",
                    "Replace the auto-bound eye-shadow overlay with solid opaque red to test "
                    "whether the head shader draws the usage-6 decal slot at all.");

struct ShaderParameterOverride {
  uint32_t usage;
  float x;
  float y;
  float z;
  float w;
};

static bool VertexToGuest(X_AVATAR_VERTEX* guest, const Vertex& host) {
  guest->position.x = host.position.x;
  guest->position.y = host.position.y;
  guest->position.z = host.position.z;
  guest->normal = host.normal;
  guest->blend_weight = host.blend_weight;
  guest->blend_indices = host.blend_indices;
  guest->color = host.color;
  for (size_t i = 0; i < host.uvs.size(); ++i) {
    auto& guest_uv = guest->uvs[i];
    const auto& host_uv = host.uvs[i];
    guest_uv.u = host_uv.x;
    guest_uv.v = host_uv.y;
  }
  return true;
}

static void OverrideShaderParameter(
    X_AVATAR_SHADER_PARAM* parameters,
    const ShaderParameterOverride& parameter_override) {
  for (size_t i = 0, o = 19; i < 20; ++i, --o) {
    auto& parameter = parameters[o];
    if (parameter.usage != parameter_override.usage) {
      continue;
    }
    assert_true(
        parameter.type == uint32_t(ShaderParameterType::kPixelConstant) ||
        parameter.type == uint32_t(ShaderParameterType::kVertexConstant));
    parameter.constant_values[0] = parameter_override.x;
    parameter.constant_values[1] = parameter_override.y;
    parameter.constant_values[2] = parameter_override.z;
    parameter.constant_values[3] = parameter_override.w;
  }
}

static bool TriangleBatchToGuest(
    X_AVATAR_TRIANGLE_BATCH& guest, const TriangleBatch& host,
    MemoryBlock* cpu_memory, uint8_t* cpu_buffer, MemoryBlock* gpu_memory,
    uint8_t* gpu_buffer, uint32_t gpu_buffer_base_ptr,
    const std::vector<ShaderParameterOverride>& shader_parameter_overrides) {
  guest.shader_id = host.shader_id;
  std::memset(guest.shader_parameters, 0, sizeof(guest.shader_parameters));
  for (size_t i = 0; i < host.shader_parameters.size(); ++i) {
    auto& guest_parameter = guest.shader_parameters[i];
    const auto& host_parameter = host.shader_parameters[i];
    guest_parameter.type = static_cast<uint32_t>(host_parameter.type);
    guest_parameter.usage = host_parameter.usage;
    if (host_parameter.type == ShaderParameterType::kTexture) {
      guest_parameter.texture.index = host_parameter.texture.index;
      guest_parameter.texture.uv_index = host_parameter.texture.uv_layer;
      guest_parameter.texture.flags = host_parameter.texture.flags;
    } else {
      for (size_t j = 0; j < 4; ++j) {
        guest_parameter.constant_values[j] = host_parameter.constant_values[j];
      }
    }
  }

  for (const auto& shader_parameter_override : shader_parameter_overrides) {
    OverrideShaderParameter(guest.shader_parameters, shader_parameter_override);
  }

  guest.triangle_count = host.triangle_count;
  guest.vertex_count = static_cast<uint32_t>(host.vertices.size());
  guest.uv_count = host.uv_count;
  guest.vertex_size = host.vertex_size;
  guest.index_size = host.index_size;
  gpu_memory->SetPointer(&guest.vertices_ptr,
                         gpu_buffer_base_ptr + host.vertex_array_offset);
  gpu_memory->SetPointer(&guest.indices_ptr,
                         gpu_buffer_base_ptr + host.index_array_offset);
  uint32_t vertex_offset = host.vertex_array_offset;
  for (size_t i = 0; i < host.vertices.size(); ++i) {
    if (!VertexToGuest(
            reinterpret_cast<X_AVATAR_VERTEX*>(&gpu_buffer[vertex_offset]),
            host.vertices[i])) {
      return false;
    }
    vertex_offset += guest.vertex_size;
  }
  uint32_t index_offset = host.index_array_offset;
  for (size_t i = 0; i < host.indices.size(); ++i) {
    auto guest_index =
        reinterpret_cast<be<uint16_t>*>(&gpu_buffer[index_offset]);
    *guest_index = host.indices[i];
    index_offset += guest.index_size;
  }
  return true;
}

static bool TextureToGuest(X_AVATAR_TEXTURE& guest, const ModelTexture& host,
                           MemoryBlock* cpu_memory, uint8_t* cpu_buffer,
                           MemoryBlock* gpu_memory, uint8_t* gpu_buffer,
                           uint32_t gpu_buffer_base_ptr) {
  auto format = host.texture.format;
  format &= ~0x00000100u;
  guest.format = format;
  guest.width = host.texture.width;
  guest.height = host.texture.height;
  guest.total_base_size = host.texture.total_data_size;
  guest.total_mip_size = 0;
  guest.base_size = host.texture.data_size;
  guest.mip_size = 0;
  guest.mip_levels = 1;
  guest.layer_count = host.texture.layer_count;
  gpu_memory->SetPointer(&guest.base_data_ptr,
                         gpu_buffer_base_ptr + host.gpu_offset);
  guest.mip_data_ptr = 0;

  auto data_buffer = &gpu_buffer[host.gpu_offset];
  // The whole slot is zeroed first so empty placeholders and the tail beyond a
  // short replacement stack decode as transparent DXT blocks.
  std::memset(data_buffer, 0, host.gpu_size);
  if (!host.texture.is_empty) {
    uint32_t output_align = (host.texture.format == 0x1A200152u ||
                             host.texture.format == 0x1A200052u)
                                ? 256u
                                : 512u;
    size_t input_stride = host.texture.data_stride;
    size_t output_stride = align(host.texture.data_stride, output_align);
    size_t input_offset = 0;
    size_t output_offset = 0;
    assert_true(host.texture.data_rows * output_stride <= host.gpu_size);
    // Layers are clamped to the model's texture slot, since a replacement
    // stack larger than the placeholder slot would overwrite neighbouring data
    // in the shared GPU buffer.
    uint32_t layer_count = host.texture.layer_count;
    const size_t layer_bytes = host.texture.data_rows * output_stride;
    if (layer_bytes && layer_count * layer_bytes > host.gpu_size) {
      const uint32_t max_layers = static_cast<uint32_t>(host.gpu_size / layer_bytes);
      REXKRNL_WARN("[avatar] texture stack ({} layers x {:#x}) exceeds its slot ({:#x}); "
                   "clamping to {} layers",
                   layer_count, layer_bytes, host.gpu_size, max_layers);
      layer_count = max_layers;
    }
    for (size_t layer = 0; layer < layer_count; ++layer) {
      for (size_t y = 0; y < host.texture.data_rows; ++y) {
        std::memcpy(&data_buffer[output_offset],
                    &host.texture.data_bytes.data()[input_offset],
                    input_stride);
        input_offset += input_stride;
        output_offset += output_stride;
      }
    }
  }
  return true;
}

static bool ModelToGuest(
    std::shared_ptr<Model> host_model, X_AVATAR_MODEL* guest_model,
    MemoryBlock* cpu_memory, MemoryBlock* gpu_memory, uint32_t category_mask,
    std::shared_ptr<Texture> replacement_textures[6],
    const std::vector<ShaderParameterOverride>& shader_parameter_overrides) {
  if (host_model == nullptr) {
    return false;
  }

  uint32_t cpu_buffer_ptr, gpu_buffer_ptr;
  uint8_t* cpu_buffer =
      cpu_memory->ClaimBytes(&cpu_buffer_ptr, host_model->cpu_size);
  uint8_t* gpu_buffer =
      gpu_memory->ClaimBytes(&gpu_buffer_ptr, host_model->gpu_size);

  *guest_model = {};
  guest_model->cpu_size = host_model->cpu_size;
  guest_model->gpu_size = host_model->gpu_size;
  guest_model->texture_size = host_model->texture_buffer_size;
  guest_model->vertex_size = host_model->vertex_buffer_size;
  guest_model->index_size = host_model->index_buffer_size;
  guest_model->triangle_batch_count =
      static_cast<uint32_t>(host_model->triangle_batches.size());
  guest_model->texture_count =
      static_cast<uint32_t>(host_model->textures.size());
  cpu_memory->SetPointer(&guest_model->cpu_buffer_ptr, cpu_buffer_ptr);
  gpu_memory->SetPointer(&guest_model->gpu_buffer_ptr, gpu_buffer_ptr);
  gpu_memory->SetPointer(&guest_model->vertex_buffer_ptr,
                         gpu_buffer_ptr + host_model->vertex_buffer_offset);
  gpu_memory->SetPointer(&guest_model->index_buffer_ptr,
                         gpu_buffer_ptr + host_model->index_buffer_offset);
  cpu_memory->SetPointer(
      &guest_model->triangle_batches_ptr,
      cpu_buffer_ptr + host_model->triangle_batch_array_offset);
  cpu_memory->SetPointer(&guest_model->textures_ptr,
                         cpu_buffer_ptr + host_model->texture_array_offset);

  auto guest_triangle_batches = reinterpret_cast<X_AVATAR_TRIANGLE_BATCH*>(
      &cpu_buffer[host_model->triangle_batch_array_offset]);
  for (size_t i = 0; i < guest_model->triangle_batch_count; ++i) {
    if (!TriangleBatchToGuest(guest_triangle_batches[i],
                              host_model->triangle_batches[i], cpu_memory,
                              cpu_buffer, gpu_memory, gpu_buffer,
                              gpu_buffer_ptr, shader_parameter_overrides)) {
      return false;
    }
  }

  // override head textures with those from the metadata
  if (category_mask == ComponentCategory::kHead) {
    int usage_indices[20];
    for (size_t i = 0; i < 20; ++i) {
      usage_indices[i] = -1;
    }
    const int usage_to_replacement_texture_indices[] = {
        -1, -1, -1, -1, -1, 5, 3, 2, 2, 1, 1, 4, 0,
    };
    std::vector<int> replacement_texture_indices(guest_model->texture_count);
    for (size_t i = 0; i < guest_model->triangle_batch_count; ++i) {
      const auto& triangle_batch = host_model->triangle_batches[i];
      for (const auto& shader_parameter : triangle_batch.shader_parameters) {
        if (shader_parameter.usage >= 20 ||
            shader_parameter.type != ShaderParameterType::kTexture) {
          continue;
        }
        const auto& usage = shader_parameter.usage;
        const auto& texture_index = shader_parameter.texture.index;
        assert_true(usage_indices[usage] == -1 ||
                    usage_indices[usage] == texture_index);
        usage_indices[usage] = texture_index;
        replacement_texture_indices[texture_index] =
            usage < countof(usage_to_replacement_texture_indices)
                ? usage_to_replacement_texture_indices[usage]
                : -1;
      }
    }

    auto guest_textures = reinterpret_cast<X_AVATAR_TEXTURE*>(
        &cpu_buffer[host_model->texture_array_offset]);
    for (size_t i = 0; i < guest_model->texture_count; ++i) {
      ModelTexture model_texture;
      auto replacement_texture_index = replacement_texture_indices[i];
      if (replacement_texture_index >= 0 &&
          replacement_textures[replacement_texture_index] != nullptr) {
        model_texture.gpu_offset = host_model->textures[i].gpu_offset;
        model_texture.gpu_size = host_model->textures[i].gpu_size;
        model_texture.texture =
            *replacement_textures[replacement_texture_index];
      } else {
        model_texture = host_model->textures[i];
      }
      if (!TextureToGuest(guest_textures[i], model_texture, cpu_memory,
                          cpu_buffer, gpu_memory, gpu_buffer, gpu_buffer_ptr)) {
        return false;
      }
    }
  } else {
    auto guest_textures = reinterpret_cast<X_AVATAR_TEXTURE*>(
        &cpu_buffer[host_model->texture_array_offset]);
    for (size_t i = 0; i < guest_model->texture_count; ++i) {
      if (!TextureToGuest(guest_textures[i], host_model->textures[i],
                          cpu_memory, cpu_buffer, gpu_memory, gpu_buffer,
                          gpu_buffer_ptr)) {
        return false;
      }
    }
  }

  return true;
}

static bool SkeletonToGuest(X_AVATAR_SKELETON* guest,
                            std::shared_ptr<Skeleton> host,
                            MemoryBlock* cpu_memory) {
  assert_true(host->joints.size() <= 72);
  uint8_t joint_count = static_cast<uint8_t>(host->joints.size());

  *guest = {};
  guest->joint_count = joint_count;

  uint32_t guest_joints_ptr;
  auto guest_joints = cpu_memory->Claim<X_AVATAR_SKELETON_JOINT>(
      &guest_joints_ptr, joint_count);
  cpu_memory->SetPointer(&guest->joints_ptr, guest_joints_ptr);

  for (uint8_t i = 0; i < joint_count; ++i) {
    auto& guest_joint = guest_joints[i];
    const auto& host_joint = host->joints[i];
    guest_joint.parent_index = host_joint.parent_index;
    guest_joint.first_child_index = host_joint.first_child_index;
    guest_joint.next_index = host_joint.next_index;
    guest_joint.bindpose.position.x = host_joint.bindpose.position.x;
    guest_joint.bindpose.position.y = host_joint.bindpose.position.y;
    guest_joint.bindpose.position.z = host_joint.bindpose.position.z;
    guest_joint.bindpose.position.w = 1.f;
    guest_joint.bindpose.rotation.x = host_joint.bindpose.rotation.x;
    guest_joint.bindpose.rotation.y = host_joint.bindpose.rotation.y;
    guest_joint.bindpose.rotation.z = host_joint.bindpose.rotation.z;
    guest_joint.bindpose.rotation.w = host_joint.bindpose.rotation.w;
    guest_joint.pose.position.x = host_joint.pose.position.x;
    guest_joint.pose.position.y = host_joint.pose.position.y;
    guest_joint.pose.position.z = host_joint.pose.position.z;
    guest_joint.pose.position.w = 1.f;
    guest_joint.pose.rotation.x = host_joint.pose.rotation.x;
    guest_joint.pose.rotation.y = host_joint.pose.rotation.y;
    guest_joint.pose.rotation.z = host_joint.pose.rotation.z;
    guest_joint.pose.rotation.w = host_joint.pose.rotation.w;
    guest_joint.pose.scale.x = host_joint.pose.scale.x;
    guest_joint.pose.scale.y = host_joint.pose.scale.y;
    guest_joint.pose.scale.z = host_joint.pose.scale.z;
    guest_joint.pose.scale.w = 1.f;
  }

  return true;
}

static void GetShaderOverrides(
    const X_AVATAR_METADATA& metadata, uint32_t category_mask,
    std::vector<ShaderParameterOverride>& shader_parameter_overrides) {
  // skin color
  if (category_mask & ComponentCategory::kBody) {
    auto color = metadata.colors[0];
    ShaderParameterOverride shader_parameter_override;
    shader_parameter_override.usage = 22;
    shader_parameter_override.x = ((color >> 16) & 0xFF) / 255.f;
    shader_parameter_override.y = ((color >> 8) & 0xFF) / 255.f;
    shader_parameter_override.z = ((color >> 0) & 0xFF) / 255.f;
    shader_parameter_override.w = ((color >> 24) & 0xFF) / 255.f;
    shader_parameter_overrides.push_back(shader_parameter_override);
  }
  // hair color
  else if (category_mask & ComponentCategory::kHair) {
    auto color = metadata.colors[1];
    ShaderParameterOverride shader_parameter_override;
    shader_parameter_override.usage = 22;
    shader_parameter_override.x = ((color >> 16) & 0xFF) / 255.f;
    shader_parameter_override.y = ((color >> 8) & 0xFF) / 255.f;
    shader_parameter_override.z = ((color >> 0) & 0xFF) / 255.f;
    shader_parameter_override.w = ((color >> 24) & 0xFF) / 255.f;
    shader_parameter_overrides.push_back(shader_parameter_override);
  } else {
    ShaderParameterOverride shader_parameter_override;
    shader_parameter_override.usage = 22;
    shader_parameter_override.x = 1.f;
    shader_parameter_override.y = 1.f;
    shader_parameter_override.z = 1.f;
    shader_parameter_override.w = 1.f;
    shader_parameter_overrides.push_back(shader_parameter_override);
  }

  if (category_mask & ComponentCategory::kHead) {
    for (uint32_t i = 0, usage = 13; i < 9; ++i, ++usage) {
      auto color = metadata.colors[i];
      ShaderParameterOverride shader_parameter_override;
      shader_parameter_override.usage = usage;
      shader_parameter_override.x = ((color >> 16) & 0xFF) / 255.f;
      shader_parameter_override.y = ((color >> 8) & 0xFF) / 255.f;
      shader_parameter_override.z = ((color >> 0) & 0xFF) / 255.f;
      shader_parameter_override.w = ((color >> 24) & 0xFF) / 255.f;
      shader_parameter_overrides.push_back(shader_parameter_override);
    }
  }

  // rim light color
  {
    auto color = metadata.colors[0];
    ShaderParameterOverride shader_parameter_override;
    shader_parameter_override.usage = 27;
    // TODO: calculate the rim light color instead of using constants.
    shader_parameter_override.x = 0.54901963f;
    shader_parameter_override.y = 0.5019608f;
    shader_parameter_override.z = 0.40392157f;
    shader_parameter_override.w = 1.6f;
    shader_parameter_overrides.push_back(shader_parameter_override);
  }
}

// A closet item supplies CUSTOM_0..2 (usages 22-24) from its colour table (STRB block 7), overriding the white tint.
static void AddItemColors(const AssetId& asset_id, std::vector<ShaderParameterOverride>& overrides) {
  std::vector<uint8_t> bytes;
  const uint8_t* table = nullptr;
  size_t size = 0;
  if (!GetCloset().ReadItemBytes(asset_id, bytes) ||
      !strb::GetSTRBBlock(bytes.data(), bytes.size(), strb::STRBBlockId::kCustomColorTable, table, size) ||
      size < 28) {
    return;
  }
  // Version word (1) then three {ARGB colour, reserved} pairs; the version word gives the byte order.
  const bool little = table[0] != 0;
  auto word = [&](size_t at) {
    const uint8_t* b = table + at;
    return little ? uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24
                  : uint32_t(b[3]) | uint32_t(b[2]) << 8 | uint32_t(b[1]) << 16 | uint32_t(b[0]) << 24;
  };
  const uint32_t colors[3] = {word(4), word(12), word(20)};
  if (!colors[0] && !colors[1] && !colors[2]) return;
  for (uint32_t i = 0; i < 3; ++i) {
    ShaderParameterOverride entry;
    entry.usage = 22 + i;
    entry.x = ((colors[i] >> 16) & 0xFF) / 255.f;
    entry.y = ((colors[i] >> 8) & 0xFF) / 255.f;
    entry.z = (colors[i] & 0xFF) / 255.f;
    entry.w = ((colors[i] >> 24) & 0xFF) / 255.f;
    overrides.push_back(entry);
  }
}

bool LoadAssetsToGuest(const X_AVATAR_METADATA& metadata,
                       uint32_t category_mask, uint32_t flags,
                       AssetPack* asset_pack, MemoryBlock* cpu_memory,
                       MemoryBlock* gpu_memory, uint32_t skeleton_version,
                       uint32_t coordinate_system) {
  bool want_prop = !!(category_mask & ComponentCategory::kProp);

  category_mask &= ~ComponentCategory::kProp;

  BlendShapeLoadOptions blend_shape_load_options = BlendShapeLoadOption::kNone;
  SkeletonLoadOptions skeleton_load_options = SkeletonLoadOption::kNone;
  ModelLoadOptions model_load_options = ModelLoadOption::kNone;
  if (coordinate_system == 0) {
    blend_shape_load_options |= BlendShapeLoadOption::kInvert;
    skeleton_load_options |= SkeletonLoadOption::kInvert;
    model_load_options |= ModelLoadOption::kInvert;
  }

  BodyType body_type = GetBodyType(metadata);

  auto skeleton = LoadSkeleton(skeleton_version, skeleton_load_options);
  if (skeleton == nullptr) {
    REXKRNL_WARN("Failed to load avatar skeleton version {}", skeleton_version);
    return false;
  }

  if (skeleton_version == 1) {
    ApplyScalesToSkeletonV1(body_type, metadata.weight_factor,
                            metadata.height_factor, skeleton);
  } else if (skeleton_version == 2) {
    ApplyScalesToSkeletonV2(body_type, metadata.weight_factor,
                            metadata.height_factor, skeleton);
  } else {
    REXKRNL_WARN("Unknown avatar skeleton version {}", skeleton_version);
    return false;
  }

  std::vector<std::pair<AssetId, std::shared_ptr<BlendShape>>> blend_shapes;
  // Bounded by blend_shapes[3], since entries past it alias textures[0..1],
  // whose ids are texture assets.
  for (size_t i = 0; i < countof(metadata.blend_shapes); ++i) {
    const auto& asset_id = metadata.blend_shapes[i].asset_id;
    if (asset_id.is_zero()) {
      continue;
    }
    auto blend_shape = LoadAsset<BlendShape, BlendShapeLoadOptions>(
        asset_pack, asset_id, blend_shape_load_options);
    if (blend_shape == nullptr) {
      REXKRNL_ERROR("Failed to load avatar blend shape {}", asset_id.to_string());
      continue;
    }
    blend_shapes.push_back({asset_id, blend_shape});
  }

  std::vector<X_AVATAR_COMPONENT_INFO> source_component_infos;
  if (metadata.body_component.matches(category_mask)) {
    source_component_infos.push_back(metadata.body_component);
  }
  if (metadata.head_component.matches(category_mask)) {
    source_component_infos.push_back(metadata.head_component);
  }
  for (const auto& component : metadata.components) {
    if (component.matches(category_mask)) {
      source_component_infos.push_back(component);
    }
  }

  // With a hat worn, the hair swaps to its companion variant, the flattened
  // "(Hat)" mesh named by asset_ids[0].b of the catalog hair entry.
  {
    bool hat_worn = false;
    for (const auto& info : source_component_infos) {
      if (info.matches(ComponentCategory::kHat)) {
        hat_worn = true;
        break;
      }
    }
    if (hat_worn && asset_pack != nullptr) {
      const auto& pack_infos = asset_pack->asset_infos();
      for (auto& info : source_component_infos) {
        if (!info.matches(ComponentCategory::kHair) || !IsStockPackId(info.asset_id)) {
          continue;
        }
        const size_t hair_index = info.asset_id.b;
        if (hair_index >= pack_infos.size()) {
          continue;
        }
        const auto& id0 = pack_infos[hair_index].asset_ids[0];
        const size_t partner_index = id0.b;
        if (id0.is_zero() || partner_index == hair_index ||
            partner_index >= pack_infos.size()) {
          continue;
        }
        info.asset_id.b = static_cast<uint16_t>(partner_index);
      }
    }
  }

  // Wearables link a body-hiding blend shape through asset_ids[0] (a =
  // 0x01000000, b = pack index of a kShapeOverrides STRB), while imported
  // marketplace items embed theirs as a kShapeOverrides block in the item's
  // own YTGR/STRB blob.
  if (asset_pack != nullptr) {
    const auto& pack_infos = asset_pack->asset_infos();
    for (const auto& info : source_component_infos) {
      if (!IsStockPackId(info.asset_id)) {
        // Closet items embed their hiding shapes, one kShapeOverrides block
        // per gender body for unisex items, so every block is loaded and the
        // apply loop matches the right one to the worn body by id.
        std::vector<uint8_t> item_bytes;
        if (!GetCloset().ReadItemBytes(info.asset_id, item_bytes) || item_bytes.empty()) {
          continue;
        }
        const size_t shape_count = strb::CountSTRBBlocks(item_bytes.data(), item_bytes.size(),
                                                         strb::STRBBlockId::kShapeOverrides);
        for (size_t occurrence = 0; occurrence < shape_count; ++occurrence) {
          const uint8_t* shape_block = nullptr;
          size_t shape_block_size = 0;
          if (!strb::GetSTRBBlockN(item_bytes.data(), item_bytes.size(),
                                   strb::STRBBlockId::kShapeOverrides, occurrence, shape_block,
                                   shape_block_size)) {
            break;
          }
          auto hiding_shape =
              BlendShape::Read(shape_block, shape_block_size, blend_shape_load_options);
          if (hiding_shape == nullptr) {
            REXKRNL_WARN("[avatar] failed to load embedded hiding shape {} for closet item {}",
                         occurrence, info.asset_id.to_string());
            continue;
          }
          // Marketplace shapes reference the body by pack index (male 0 /
          // female 1) where manifests use the canonical {a=2, b=0, c=gender}.
          if (hiding_shape->index_patch.original_asset_id.a.get() == 2) {
            hiding_shape->index_patch.original_asset_id.b = 0;
          }
          if (hiding_shape->vertex_patch.original_asset_id.a.get() == 2) {
            hiding_shape->vertex_patch.original_asset_id.b = 0;
          }
          blend_shapes.push_back({info.asset_id, hiding_shape});
        }
        continue;
      }
      const size_t pack_index = info.asset_id.b;
      if (pack_index >= pack_infos.size()) {
        continue;
      }
      const auto& id0 = pack_infos[pack_index].asset_ids[0];
      if (id0.is_zero() || id0.a.get() != 0x01000000u || id0.b.get() >= pack_infos.size()) {
        continue;
      }
      auto hiding_shape = LoadAsset<BlendShape, BlendShapeLoadOptions>(
          asset_pack, id0, blend_shape_load_options);
      if (hiding_shape == nullptr) {
        REXKRNL_WARN("[avatar] failed to load hiding shape {} for component {}",
                     id0.to_string(), info.asset_id.to_string());
        continue;
      }
      blend_shapes.push_back({id0, hiding_shape});
    }
  }

  std::vector<std::pair<X_AVATAR_COMPONENT_INFO, std::shared_ptr<Model>>>
      source_components;
  for (const auto& source_info : source_component_infos) {
    // The Avatar Editor stores face-morph selections as manifest components
    // whose pack assets are shape-override STRBs (the 0x01000000-category
    // population carries no model blocks), so they are routed into the
    // blend-shape set.
    {
      const uint8_t* raw_buffer = nullptr;
      size_t raw_size = 0;
      std::vector<uint8_t> raw_temp;
      if (LoadAsset(asset_pack, source_info.asset_id, raw_buffer, raw_size, raw_temp) &&
          strb::CountSTRBBlocks(raw_buffer, raw_size,
                                strb::STRBBlockId::kShapeOverrides) > 0 &&
          strb::CountSTRBBlocks(raw_buffer, raw_size, strb::STRBBlockId::kModel) == 0) {
        auto component_shape = BlendShape::Load(raw_buffer, raw_size,
                                                blend_shape_load_options);
        if (component_shape != nullptr) {
          blend_shapes.push_back({source_info.asset_id, component_shape});
        } else {
          REXKRNL_ERROR("Failed to load component blend shape {}",
                        source_info.asset_id.to_string());
        }
        continue;
      }
    }
    auto model = LoadAsset<Model, ModelLoadOptions>(
        asset_pack, source_info.asset_id, model_load_options);
    if (model != nullptr) {
      source_components.push_back({source_info, model});
      continue;
    }
    REXKRNL_ERROR("Failed to load avatar asset {}, looking for a fallback",
           source_info.asset_id.to_string());
    X_AVATAR_COMPONENT_INFO fallback_info{};
    for (const auto& candidate_info : metadata.fallback_components) {
      if (candidate_info.categories == source_info.categories) {
        // TODO: no further fallback when this load fails.
        model = LoadAsset<Model, ModelLoadOptions>(
            asset_pack, candidate_info.asset_id, model_load_options);
        fallback_info = candidate_info;
        break;
      }
    }
    if (model == nullptr) {
      if (!fallback_info.asset_id.is_zero()) {
        REXKRNL_ERROR("Failed to load fallback avatar asset {}",
               fallback_info.asset_id.to_string());
      }
      continue;
    }
    source_components.push_back({fallback_info, model});
  }

  std::shared_ptr<Texture> replacement_textures[6];
  // metadata.textures[] slots have fixed meanings (slot -> usage: 0 -> 12
  // mouth, 1 -> 9/10 eyes, 2 -> 7/8 eyebrows, 3 -> 6 face paint, 4 -> 11 eye
  // shadow, 5 -> 5 face texture), so entries whose category is valid for their
  // slot stay and mismatched entries relocate to the first free slot that
  // accepts their category.
  static const uint32_t kSlotAcceptedCategories[6][2] = {
      {0x8000, 0},        // mouth
      {0x2000, 0},        // eyes
      {0x4000, 0},        // brows
      {0x40000, 0x10000}, // face paint
      {0x40000, 0x10000}, // eye shadow
      {0x20000, 0x10000}, // face texture
  };
  auto slot_accepts = [&](size_t slot, uint32_t category) {
    return kSlotAcceptedCategories[slot][0] == category ||
           kSlotAcceptedCategories[slot][1] == category;
  };
  std::shared_ptr<Texture> loaded_textures[6];
  size_t slot_source[6] = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
  for (size_t i = 0; i < 6; ++i) {
    const auto& texture_info = metadata.textures[i];
    if (texture_info.asset_id.is_zero()) {
      continue;
    }
    loaded_textures[i] = LoadAsset<Texture>(asset_pack, texture_info.asset_id);
    if (loaded_textures[i] == nullptr) {
      REXKRNL_ERROR("Failed to load avatar replacement texture {}",
             texture_info.asset_id.to_string());
      continue;
    }
    // Pass 1: entries already in a slot that accepts their category.
    if (slot_accepts(i, texture_info.asset_id.a.get())) {
      replacement_textures[i] = loaded_textures[i];
      slot_source[i] = i;
      loaded_textures[i] = nullptr;
    }
  }
  for (size_t i = 0; i < 6; ++i) {
    // Pass 2: category-mismatched leftovers relocate to a free accepting slot.
    if (loaded_textures[i] == nullptr) {
      continue;
    }
    const uint32_t id_category = metadata.textures[i].asset_id.a.get();
    for (size_t s = 0; s < 6; ++s) {
      if (replacement_textures[s] == nullptr && slot_accepts(s, id_category)) {
        REXKRNL_INFO("[avatar] replacement texture {} (cat {:#x}) reslotted {} -> {}",
                     metadata.textures[i].asset_id.to_string(), id_category, i, s);
        replacement_textures[s] = loaded_textures[i];
        slot_source[s] = i;
        break;
      }
    }
  }
  // The eye-shadow overlay (slot 4 / usage 11) is not stored in the manifest,
  // so the worn eyes stack (0x2000) binds its paired shadow entry through
  // asset_ids[0] (a = 0x40000) and metadata.colors[5] tints it.
  if (replacement_textures[4] == nullptr && replacement_textures[1] != nullptr &&
      slot_source[1] != SIZE_MAX && asset_pack != nullptr &&
      IsStockPackId(metadata.textures[slot_source[1]].asset_id)) {
    const auto& eye_id = metadata.textures[slot_source[1]].asset_id;
    const auto& pack_infos = asset_pack->asset_infos();
    const size_t eye_index = eye_id.b;
    if (eye_index < pack_infos.size()) {
      const auto& id0 = pack_infos[eye_index].asset_ids[0];
      if (!id0.is_zero() && id0.a.get() == 0x40000u && id0.b.get() < pack_infos.size()) {
        auto shadow = LoadAsset<Texture>(asset_pack, id0);
        if (shadow != nullptr) {
          if (REXCVAR_GET(avatar_eyeshadow_debug) && !shadow->data_bytes.empty()) {
            // Solid opaque red DXT5 block, bytes pre-swapped for the 8-in-16
            // fetch endian, that makes the overlay's coverage visible.
            static const uint8_t kRedBlock[16] = {0xFF, 0xFF, 0, 0, 0, 0, 0, 0,
                                                  0xF8, 0x00, 0xF8, 0x00, 0, 0, 0, 0};
            for (size_t off = 0; off + 16 <= shadow->data_bytes.size(); off += 16) {
              std::memcpy(&shadow->data_bytes[off], kRedBlock, 16);
            }
            REXKRNL_INFO("[avatar] eyeshadow debug fill active");
          }
          replacement_textures[4] = shadow;
        }
      }
    }
  }

  // Old-body item rescue (pre-Fall-2010 authored items) runs before the apply
  // loop and remaps each stale body-targeting shape and its owning garment's
  // mesh into current body space (see RescueOldBodyItem).
  for (auto& blend_shape : blend_shapes) {
    if (blend_shape.second->vertex_patch.original_asset_id.a.get() != 2u) {
      continue;
    }
    for (auto& body_candidate : source_components) {
      if (!blend_shape.second->matches(body_candidate.first.asset_id)) {
        continue;
      }
      std::shared_ptr<Model> owner_model;
      for (auto& owner_candidate : source_components) {
        if (owner_candidate.first.asset_id == blend_shape.first) {
          owner_model = owner_candidate.second;
          break;
        }
      }
      RescueOldBodyItem(blend_shape.second, body_candidate.second, owner_model,
                        LoadLegacyBody(blend_shape.second->vertex_patch.original_asset_id.c,
                                       model_load_options));
      break;
    }
  }

  for (const auto& source_component : source_components) {
    for (const auto& blend_shape : blend_shapes) {
      if (!blend_shape.second->matches(source_component.first.asset_id)) {
        continue;
      }
      if (!ApplyBlendShape(blend_shape.second, source_component.first.asset_id,
                           source_component.second)) {
        REXKRNL_ERROR("Failed to apply blend shape {} to asset {}",
               blend_shape.first.to_string(),
               source_component.first.asset_id.to_string());
      }
    }
  }

  // Native custom-avatar substitution swaps the largest decoded component's
  // model for the custom mesh and reduces the others to zero triangles,
  // returning -1 when nothing is substituted.
  int custom_component_index = ApplyCustomAvatar(source_components);

  // AI special-asset gear (a DCC2 ai-assets STRB queued per thread by
  // XamAvatarGetAssets) is appended as an extra component and serialized
  // category-neutral like the custom avatar.
  int extra_component_index =
      AppendPendingExtraComponent(source_components, model_load_options);

  std::shared_ptr<Prop> prop = nullptr;
  X_AVATAR_COMPONENT_INFO prop_info{};
  if (want_prop) {
    for (const auto& component : metadata.components) {
      if (component.matches(ComponentCategory::kProp)) {
        prop_info = component;
        break;
      }
    }
    if (!prop_info.asset_id.is_zero()) {
      PropLoadOptions prop_load_options{};
      prop_load_options.model = model_load_options;
      prop_load_options.skeleton = skeleton_load_options;
      prop_load_options.animation = AnimationLoadOption::kGuest;
      prop_load_options.blend_shape = blend_shape_load_options;
      if (coordinate_system == 0) {
        prop_load_options.animation |= AnimationLoadOption::kInvert;
      }
      prop_load_options.model = model_load_options;
      prop = LoadAsset<Prop, PropLoadOptions>(asset_pack, prop_info.asset_id,
                                              prop_load_options);
    }
  }

  auto assets = cpu_memory->Claim<X_AVATAR_ASSETS>();
  *assets = {};

  uint32_t guest_skeleton_ptr;
  auto guest_skeleton =
      cpu_memory->Claim<X_AVATAR_SKELETON>(&guest_skeleton_ptr);

  if (!SkeletonToGuest(guest_skeleton, skeleton, cpu_memory)) {
    return false;
  }

  cpu_memory->SetPointer(&assets->skeleton_ptr, guest_skeleton_ptr);

  if (source_components.size()) {
    uint32_t component_infos_ptr;
    auto component_infos = cpu_memory->Claim<X_AVATAR_COMPONENT_INFO>(
        &component_infos_ptr, source_components.size());
    uint32_t component_models_ptr;
    auto component_models = cpu_memory->Claim<X_AVATAR_MODEL>(
        &component_models_ptr, source_components.size());

    assets->component_count = uint32_t(source_components.size());

    cpu_memory->SetPointer(&assets->component_infos_ptr, component_infos_ptr);
    cpu_memory->SetPointer(&assets->component_models_ptr, component_models_ptr);

    auto component_info = component_infos;
    auto component_model = component_models;
    int component_index = 0;
    for (const auto& source_component : source_components) {
      *component_info = source_component.first;

      // The substituted and appended components serialize category-neutral:
      // GetShaderOverrides(kNone) forces the usage-22 tint constant to white
      // and ModelToGuest(kNone) skips the head texture-replacement path, while
      // the guest-visible component_info categories stay untouched.
      const bool is_custom = component_index == custom_component_index ||
                             component_index == extra_component_index;
      const uint32_t effective_categories =
          is_custom ? ComponentCategory::kNone
                    : uint32_t(source_component.first.categories);

      std::vector<ShaderParameterOverride> shader_parameter_overrides;
      GetShaderOverrides(metadata, effective_categories,
                         shader_parameter_overrides);
      // Later overrides win, so the item colours replace the tint.
      if (!is_custom && !IsStockPackId(source_component.first.asset_id)) {
        AddItemColors(source_component.first.asset_id, shader_parameter_overrides);
      }

      ModelToGuest(source_component.second, component_model, cpu_memory,
                   gpu_memory, effective_categories,
                   replacement_textures, shader_parameter_overrides);
      component_info++;
      component_model++;
      component_index++;
    }
  } else {
    assets->component_count = 0;
  }

  if (prop != nullptr) {
    uint32_t guest_prop_ptr;
    auto guest_prop = cpu_memory->Claim<X_AVATAR_PROP>(&guest_prop_ptr);

    *guest_prop = {};
    guest_prop->component_info = prop_info;

    uint32_t guest_prop_skeleton_ptr;
    auto guest_prop_skeleton =
        cpu_memory->Claim<X_AVATAR_SKELETON>(&guest_prop_skeleton_ptr);

    if (!SkeletonToGuest(guest_prop_skeleton, prop->skeleton, cpu_memory)) {
      return false;
    }

    cpu_memory->SetPointer(&guest_prop->skeleton_ptr, guest_prop_skeleton_ptr);

    if (!ModelToGuest(prop->model, &guest_prop->component_model, cpu_memory,
                      gpu_memory, prop_info.categories, {}, {})) {
      return false;
    }

    if (prop->animation != nullptr) {
      uint32_t guest_animation_ptr;
      auto guest_animation =
          cpu_memory->Claim<X_AVATAR_ANIMATION>(&guest_animation_ptr);

      guest_animation->compressed_data_size =
          static_cast<uint32_t>(prop->animation->compressed_data_bytes.size());

      uint32_t guest_compressed_data_buffer_ptr;
      auto guest_compressed_data_buffer =
          cpu_memory->ClaimBytes(&guest_compressed_data_buffer_ptr,
                                 guest_animation->compressed_data_size);

      if (!LoadAnimationToGuest(prop_info.asset_id, prop->animation,
                                guest_animation,
                                guest_compressed_data_buffer)) {
        return false;
      }

      cpu_memory->SetPointer(&guest_animation->compressed_data_buffer_ptr,
                             guest_compressed_data_buffer_ptr);

      cpu_memory->SetPointer(&guest_prop->animation_ptr, guest_animation_ptr);
    }

    cpu_memory->SetPointer(&assets->prop_ptr, guest_prop_ptr);
  }

  return true;
}

}  // namespace avatars
}  // namespace rex
