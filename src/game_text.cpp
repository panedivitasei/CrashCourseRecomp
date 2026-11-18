// game_text.cpp: corrections to the game's own text.

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_82954990);  // UNISTRINGSMASTER: a string's text in one language (master, column, text)

namespace cc1 {
namespace {

// Swaps the transposed letters at the start of messages/USER_MUST_CHOOSE_DEVICE.
void FixContinue(uint32_t holder, uint8_t* base) {
  static constexpr char kWrong[] = "To conitnue,";
  const uint32_t text = holder ? rex::memory::load_and_swap<uint32_t>(base + holder) : 0;
  if (!text) return;
  for (uint32_t i = 0; kWrong[i]; ++i) {
    if (rex::memory::load_and_swap<uint16_t>(base + text + 2 * i) != uint16_t(kWrong[i])) return;
  }
  rex::memory::store_and_swap<uint16_t>(base + text + 12, uint16_t('t'));
  rex::memory::store_and_swap<uint16_t>(base + text + 14, uint16_t('i'));
  REXLOG_INFO("[text] corrected \"To conitnue\" in the storage device message");
}

}  // namespace
}  // namespace cc1

REX_HOOK_RAW(sub_82954990) {
  cc1::FixContinue(ctx.r5.u32, base);
  __imp__sub_82954990(ctx, base);
}
