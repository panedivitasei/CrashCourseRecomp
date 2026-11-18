// content_seed.cpp: copies the City Lights pack from the game folder into the user data root, where the game lists
// marketplace content, and writes its header record.

#include "content_seed.h"

#include <cstdint>
#include <string>
#include <system_error>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xtypes.h>

namespace cc1 {
namespace {

constexpr uint32_t kTitle = 0x58410A71;
constexpr char kCommon[] = "0000000000000000";  // content every player has
constexpr char kTitleFolder[] = "58410A71";
constexpr char kMarketplace[] = "00000002";
constexpr uint32_t kOwned = 1;  // licence mask of a pack, bit 0: bought

}  // namespace

void SeedContent(const std::filesystem::path& game_root, const std::filesystem::path& user_root) {
  namespace fs = std::filesystem;
  if (game_root.empty() || user_root.empty()) return;
  std::error_code ec;
  const auto missing = fs::copy_options::recursive | fs::copy_options::skip_existing;
  const fs::path title = user_root / kCommon / kTitleFolder;
  const fs::path packs = title / kMarketplace;

  if (const fs::path tree = game_root / "DLC"; fs::is_directory(tree, ec)) {
    fs::create_directories(user_root, ec);
    fs::copy(tree, user_root, missing, ec);
    if (ec) REXLOG_WARN("[seed] DLC not copied: {}", ec.message());
  }
  const fs::path pack = game_root / "citylights";
  if (fs::is_regular_file(pack / "dlc.pak", ec) && !fs::exists(packs / "citylights" / "dlc.pak", ec)) {
    fs::create_directories(packs / "citylights", ec);
    fs::copy(pack, packs / "citylights", missing, ec);
    if (ec) {
      REXLOG_WARN("[seed] citylights not copied: {}", ec.message());
    } else {
      REXLOG_INFO("[seed] citylights copied into {}", packs.string());
    }
  }

  auto* kernel = REX_KERNEL_STATE();
  if (!kernel || !kernel->content_manager() || !fs::is_directory(packs, ec)) return;
  for (const auto& entry : fs::directory_iterator(packs, ec)) {
    if (!entry.is_directory(ec)) continue;
    const std::string name = entry.path().filename().string();
    // A header without the licence word after the record leaves the pack a demo, so it is written again.
    const fs::path header = title / "Headers" / kMarketplace / (name + ".header");
    const auto size = fs::file_size(header, ec);
    if (!ec && size >= sizeof(rex::system::xam::XCONTENT_AGGREGATE_DATA) + sizeof(uint32_t)) continue;
    rex::system::xam::XCONTENT_AGGREGATE_DATA record;
    record.device_id = 1;  // the hard drive
    record.content_type = rex::system::XContentType::kMarketplaceContent;
    record.set_display_name(std::u16string(name.begin(), name.end()));
    record.set_file_name(name);
    record.title_id = kTitle;
    record.xuid = 0;
    if (kernel->content_manager()->WriteContentHeaderFile(0, record, kOwned) != 0) {
      REXLOG_WARN("[seed] no record written for {}", name);
    }
  }
}

}  // namespace cc1
