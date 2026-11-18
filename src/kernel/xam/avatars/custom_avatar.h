// Native custom-avatar substitution: replaces the decoded default Xbox avatar
// with the avatar editor's active custom avatar
// (<game_data_root>/avatars/<active>/model_baked.obj plus .rgba texture
// sidecars) inside the XamAvatarGetAssets pipeline, gated by the
// dcc2_custom_avatar_native cvar.

#ifndef REX_AVATARS_CUSTOM_AVATAR_H_
#define REX_AVATARS_CUSTOM_AVATAR_H_

#include <memory>
#include <utility>
#include <vector>

#include "guest_asset.h"
#include "model.h"

#include "xe_compat.h"

namespace rex {
namespace avatars {

// True when the feature cvar is on and the avatar editor's active custom
// avatar exists on disk and parsed successfully (loads + caches on first use).
bool CustomAvatarActive();

// Extra guest bytes XamAvatarGetAssetsResultSize adds to the cpu / gpu buffer
// sizes when a custom avatar is active (0/0 when inactive), computed with a
// worst-case vertex layout plus slack so the ApplyCustomAvatar output fits.
void GetCustomAvatarExtraSizes(uint32_t* extra_cpu_size,
                               uint32_t* extra_gpu_size);

// Substitutes the custom avatar into the decoded component list in place,
// before guest serialization, and returns the substituted component's index
// (to be serialized with neutral shader overrides, ComponentCategory::kNone)
// or -1 when nothing is substituted.
int ApplyCustomAvatar(
    std::vector<std::pair<X_AVATAR_COMPONENT_INFO, std::shared_ptr<Model>>>&
        source_components);

// Suppresses ApplyCustomAvatar for the current thread's XamAvatarGetAssets
// build, so AI/NPC avatar builds keep their own bodies.
void SetCustomAvatarSuppressed(bool suppressed);

// Queues a raw STRB avatar-asset blob (e.g. one of DCC2's
// data/art/_avatar/ai-assets/*.bin looks) to be appended as an extra component
// in the current thread's next LoadAssetsToGuest build, with nullptr/0
// clearing it.
void SetPendingExtraComponentStrb(const uint8_t* data, size_t size);

// Decodes the queued STRB, if any, into a Model appended to the component list
// and returns its index (serialized category-neutral like the custom avatar),
// or -1 when nothing is queued or the decode fails.
int AppendPendingExtraComponent(
    std::vector<std::pair<X_AVATAR_COMPONENT_INFO, std::shared_ptr<Model>>>&
        source_components,
    ModelLoadOptions model_load_options);

}  // namespace avatars
}  // namespace rex

#endif  // REX_AVATARS_CUSTOM_AVATAR_H_
