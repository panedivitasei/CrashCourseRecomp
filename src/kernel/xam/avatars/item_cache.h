// item_cache.h: other players' marketplace items, downloaded by id from the avatar marketplace server into
// avatar_cache_dir for drawing only, never added to the closet.
#ifndef REX_AVATARS_ITEM_CACHE_H_
#define REX_AVATARS_ITEM_CACHE_H_

#include <cstdint>
#include <vector>

#include "asset_pack.h"
#include "guest_asset.h"

namespace rex {
namespace avatars {
namespace item_cache {

// Downloads the manifest items that neither the closet nor the cache holds, waiting at most timeout_ms in all.
void Fetch(const X_AVATAR_METADATA& metadata, int timeout_ms);

// Reads a downloaded item; false when it is not in the cache.
bool Read(const AssetId& id, std::vector<uint8_t>& out);

}  // namespace item_cache
}  // namespace avatars
}  // namespace rex

#endif  // REX_AVATARS_ITEM_CACHE_H_
