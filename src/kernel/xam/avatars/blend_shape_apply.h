/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_BLEND_SHAPE_APPLY_H_
#define XENIA_AVATARS_BLEND_SHAPE_APPLY_H_

#include <memory>
#include <vector>

#include "asset_pack.h"
#include "blend_shape.h"
#include "common.h"
#include "model.h"
#include <rex/types.h>

#include "xe_compat.h"

namespace rex {
namespace avatars {

bool ApplyBlendShape(std::shared_ptr<BlendShape> blend_shape,
                     const AssetId& model_asset_id,
                     std::shared_ptr<Model> model);

// Detects and repairs an item authored against the pre-Fall-2010 body
// revision, whose garment mesh and absolute tuck positions live in the old
// ~10%-smaller body space, exactly through legacy_body_model (the
// AvatarAssetPackLegacyV1 body, same topology) when available and by a
// per-axis affine fit otherwise.
bool RescueOldBodyItem(std::shared_ptr<BlendShape> blend_shape,
                       std::shared_ptr<Model> body_model,
                       std::shared_ptr<Model> item_model,
                       std::shared_ptr<Model> legacy_body_model);

}  // namespace avatars
}  // namespace rex

#endif  // XENIA_AVATARS_BLEND_SHAPE_APPLY_H_
