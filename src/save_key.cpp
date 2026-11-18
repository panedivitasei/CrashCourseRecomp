// Save key: the game encrypts its save with a key made from the player's XUID, so the key is always made from
// kSaveXuid whatever XUID the game is given, and a save keyed to another given XUID still loads.

#include <array>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

REX_EXTERN(__imp__sub_8297B020);  // NETPLAYERPROFILEXA: makes the save's cipher from the player's XUID (player)
REX_EXTERN(__imp__sub_8297B1C0);  // whether a save block's hash is its data's (block)
REX_EXTERN(__imp__sub_8297C288);  // decrypts a save block that was read, checks it and takes it (block, player, flag)
REX_EXTERN(__imp__sub_829F9C28);  // decrypts (cipher, data, size)

namespace cc1 {
namespace {

constexpr uint64_t kSaveXuid = 0x0009BABEBABEBABEull;
// NETPLAYERPROFILEXA: the player's XUID and the save's cipher.
constexpr uint32_t kXuid = 2168, kCipher = 5712;
// A save block: a magic, the data's hash, the data, and whether the data is encrypted.
constexpr uint32_t kBlockSize = 2932, kData = 24, kDataSize = 2904, kEncrypted = 2928;

// Makes the player's cipher as if their XUID were this one.
void MakeCipher(PPCContext& ctx, uint8_t* base, uint32_t player, uint64_t xuid) {
  uint8_t* field = base + player + kXuid;
  const uint64_t given = rex::memory::load_and_swap<uint64_t>(field);
  if (given) rex::memory::store_and_swap<uint64_t>(field, xuid);  // zero (nobody signed in) stays zero: no cipher
  ctx.r3.u64 = player;
  __imp__sub_8297B020(ctx, base);
  rex::memory::store_and_swap<uint64_t>(field, given);
}

// Whether the player's cipher opens the block; the block is left as it was.
bool Opens(PPCContext& ctx, uint8_t* base, uint32_t block, uint32_t player) {
  std::array<uint8_t, kBlockSize> kept;
  std::memcpy(kept.data(), base + block, kBlockSize);
  ctx.r3.u64 = player + kCipher;
  ctx.r4.u64 = block + kData;
  ctx.r5.u64 = kDataSize;
  __imp__sub_829F9C28(ctx, base);
  ctx.r3.u64 = block;
  __imp__sub_8297B1C0(ctx, base);
  const bool opens = ctx.r3.u8 != 0;
  std::memcpy(base + block, kept.data(), kBlockSize);
  return opens;
}

// Takes a block that was read, retrying one the cipher does not open with the XUIDs the game has been given.
void TakeBlock(PPCContext& ctx, uint8_t* base) {
  const uint32_t block = ctx.r3.u32, player = ctx.r4.u32;
  const uint64_t flag = ctx.r5.u64;
  bool other = false;
  if (base[block + kEncrypted] && !Opens(ctx, base, block, player)) {
    const uint64_t given = rex::memory::load_and_swap<uint64_t>(base + player + kXuid);
    for (const uint64_t xuid : {REX_KERNEL_STATE()->user_profile()->xuid(), given}) {
      if (!xuid || xuid == kSaveXuid) continue;
      MakeCipher(ctx, base, player, xuid);
      other = Opens(ctx, base, block, player);
      if (other) {
        REXLOG_INFO("[save] the save has the key of {:016X}: loaded, and written with the first releases' from now", xuid);
        break;
      }
    }
    if (!other) {
      MakeCipher(ctx, base, player, kSaveXuid);
      REXLOG_INFO("[save] the save opens with none of the keys: the game takes it as damaged");
    }
  }
  ctx.r3.u64 = block;
  ctx.r4.u64 = player;
  ctx.r5.u64 = flag;
  __imp__sub_8297C288(ctx, base);
  if (!other) return;
  const uint64_t result = ctx.r3.u64;
  MakeCipher(ctx, base, player, kSaveXuid);
  ctx.r3.u64 = result;
}

}  // namespace
}  // namespace cc1

REX_HOOK_RAW(sub_8297B020) { cc1::MakeCipher(ctx, base, ctx.r3.u32, cc1::kSaveXuid); }
REX_HOOK_RAW(sub_8297C288) { cc1::TakeBlock(ctx, base); }
