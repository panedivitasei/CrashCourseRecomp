// content_seed.h: seeds the City Lights pack into the user data root.

#pragma once

#include <filesystem>

namespace cc1 {

// Copies the pack from the game folder into the user data root where it is missing, with its record.
void SeedContent(const std::filesystem::path& game_root, const std::filesystem::path& user_root);

}  // namespace cc1
