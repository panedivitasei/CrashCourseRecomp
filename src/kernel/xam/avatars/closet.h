// Avatar closet: a directory of imported Xbox LIVE marketplace avatar items,
// one <product-guid>.bin (raw YTGR/STRB blob) per item plus closet_index.tsv
// (guid TAB categories-hex TAB bodies TAB name), resolved by product GUID.

#ifndef REX_AVATARS_CLOSET_H_
#define REX_AVATARS_CLOSET_H_

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "asset_pack.h"

namespace rex {
namespace avatars {

struct ClosetItem {
  AssetId id;
  uint32_t categories;
  uint8_t bodies;
  std::string name;  // UTF-8 display name for the selection grids
};

class Closet {
 public:
  // Loads closet_index.tsv from the directory once, treating a missing
  // directory or index as an empty closet.
  bool Load(const std::filesystem::path& dir);

  bool is_loaded() const { return is_loaded_; }
  const std::vector<ClosetItem>& items() const { return items_; }

  const ClosetItem* Find(const AssetId& id) const;

  // Reads the item's raw YTGR/STRB blob from disk.
  bool ReadItemBytes(const AssetId& id, std::vector<uint8_t>& out) const;

 private:
  bool is_loaded_ = false;
  std::filesystem::path dir_;
  std::vector<ClosetItem> items_;
  std::unordered_map<std::string, size_t> by_guid_;  // guid string -> index
};

// Global closet instance, loaded next to the avatar asset pack.
Closet& GetCloset();

// True when the id carries the stock asset pack's GUID tail (resolvable by
// pack index); false for marketplace/closet product ids.
bool IsStockPackId(const AssetId& id);

}  // namespace avatars
}  // namespace rex

#endif  // REX_AVATARS_CLOSET_H_
