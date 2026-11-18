// game_lua.cpp: runs registered Lua scripts on the game's GUI Lua state (see game_lua.h).

#include "game_lua.h"

#include "mouse.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

// The game's Lua 5.1 C API (TU1) and its per-frame GUI update.
REX_EXTERN(sub_824C02C0);         // luaL_loadbuffer(L, buffer, size, chunk name)
REX_EXTERN(sub_824BEF00);         // lua_pcall(L, nargs, nresults, errfunc)
REX_EXTERN(sub_824BE118);         // lua_tolstring(L, index, size_t* length)
REX_EXTERN(sub_824BDA38);         // lua_settop(L, index)
REX_EXTERN(sub_824BDA20);         // lua_gettop(L)
REX_EXTERN(__imp__sub_8296E0F8);  // GUI update: steps every displayed group

namespace cc1::lua {
namespace {

// The first script defines CC1_Hook, one wrapper per game global shared by every
// script's hook, and CC1_Rehook, which restores the wrappers each frame after the
// game's global script redefines them.
constexpr char kHooksScript[] = R"lua(
if rawget(_G, "CC1H") then return "have" end
local H = { original = {}, wrappers = {}, chains = {} }
rawset(_G, "CC1H", H)

-- fn(next, ...) runs in place of the global 'name', and next(...) continues the
-- chain, ending in the game's own function.
rawset(_G, "CC1_Hook", function(name, fn)
  local chain = H.chains[name]
  if not chain then
    chain = {}
    H.chains[name] = chain
    local function step(i, ...)
      local hook = chain[i]
      if not hook then return H.original[name](...) end
      return hook(function(...) return step(i + 1, ...) end, ...)
    end
    H.wrappers[name] = function(...) return step(1, ...) end
  end
  chain[#chain + 1] = fn
end)

rawset(_G, "CC1_Rehook", function()
  for name, wrapper in pairs(H.wrappers) do
    local now = rawget(_G, name)
    if now ~= wrapper then
      if now == nil then now = _G[name] end
      if now ~= nil then
        H.original[name] = now
        rawset(_G, name, wrapper)
      end
    end
  end
end)

return "installed"
)lua";

struct Entry {
  Script script;
  uint32_t installed_state = 0;  // the lua_State it is installed in
};
std::vector<Entry> g_scripts = {{{"hooks", kHooksScript, "CC1_Rehook()", nullptr, nullptr}, 0}};

// The guest call in progress, set for the length of a tick so Run needs no context arguments.
PPCContext* g_ctx = nullptr;
uint8_t* g_base = nullptr;
uint32_t g_state = 0;

uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t raw;
  std::memcpy(&raw, base + address, 4);
  return __builtin_bswap32(raw);
}

// CMDMGR (global) -> its Lua interpreter -> lua_State; 0 until the game has one.
uint32_t LuaState(const uint8_t* base) {
  constexpr uint32_t kCmdMgr = 0x82C51164;
  const uint32_t manager = Read32(base, kCmdMgr);
  const uint32_t interpreter = manager ? Read32(base, manager + 8) : 0;
  return interpreter ? Read32(base, interpreter + 8) : 0;
}

// Development console: runs a Lua chunk dropped into cc1_lua.txt beside the exe, or sets
// a setting from "cvar <name> <value>", then logs the result and removes the file.
void RunDropFile() {
  static uint32_t ticks = 0;
  if (++ticks % 30) return;
  const auto path = rex::filesystem::GetExecutableFolder() / "cc1_lua.txt";
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return;
  std::string source;
  {
    std::ifstream in(path, std::ios::binary);
    source.assign(std::istreambuf_iterator<char>(in), {});
  }
  std::filesystem::remove(path, ec);
  if (ec) return;  // still being written
  // "cvar <name> <value>" sets a setting instead.
  if (source.rfind("cvar ", 0) == 0) {
    while (!source.empty() && (source.back() == '\n' || source.back() == '\r' || source.back() == ' ')) {
      source.pop_back();
    }
    const size_t space = source.find(' ', 5);
    if (space != std::string::npos) {
      const std::string name = source.substr(5, space - 5), value = source.substr(space + 1);
      rex::cvar::SetFlagByName(name, value);
      REXLOG_INFO("[lua] cc1_lua.txt: {} = {}", name, value);
    }
    return;
  }
  std::string out;
  const bool ok = Run(source, &out);
  REXLOG_INFO("[lua] cc1_lua.txt {}: {}", ok ? "->" : "FAILED", out);
}

// Once per GUI update, on the game's main thread.
void Tick(PPCContext& ctx, uint8_t* base) {
  const uint32_t L = LuaState(base);
  if (!L) return;
  g_ctx = &ctx;
  g_base = base;
  g_state = L;
  std::string out;
  for (Entry& entry : g_scripts) {
    const Script& script = entry.script;
    if (entry.installed_state != L) {
      if (!Run(script.install, &out)) {
        LogError(script.name, out);
        continue;
      }
      if (out == "wait") continue;  // the game's global scripts have not run yet
      entry.installed_state = L;
      REXLOG_INFO("[lua] {} script {} (lua_State {:08X})", script.name, out, L);
      if (script.on_installed) script.on_installed();
    }
    if (!Run(script.poll, &out)) {
      LogError(script.name, out);
      entry.installed_state = 0;  // the Lua state was replaced: installed again next tick
      continue;
    }
    if (script.on_events) script.on_events(out);
  }
  RunDropFile();
  g_ctx = nullptr;
}

}  // namespace

void Register(const Script& script) { g_scripts.push_back({script, 0}); }

void LogError(const char* what, const std::string& message) {
  static int budget = 16;
  if (budget > 0) {
    --budget;
    REXLOG_WARN("[lua] {}: {}", what, message);
  }
}

bool Run(std::string_view source, std::string* result) {
  if (result) result->clear();
  if (!g_ctx) return false;
  PPCContext& ctx = *g_ctx;
  uint8_t* base = g_base;
  const uint32_t L = g_state;
  constexpr uint32_t kBufferSize = 48 * 1024;
  static uint32_t buffer = 0;  // guest memory for the chunk text and its name
  if (!buffer) buffer = REX_KERNEL_MEMORY()->SystemHeapAlloc(kBufferSize);
  if (!buffer || source.size() + 32 > kBufferSize) return false;
  const uint32_t name = buffer + kBufferSize - 16;
  std::memcpy(base + buffer, source.data(), source.size());
  std::memcpy(base + name, "=cc1", 5);

  const PPCContext saved = ctx;
  ctx.r1.u32 = (ctx.r1.u32 - 0x200) & ~0xFu;  // a frame clear of the caller's
  ctx.r3.u64 = L;
  sub_824BDA20(ctx, base);
  const uint32_t top = ctx.r3.u32;

  ctx.r3.u64 = L;
  ctx.r4.u64 = buffer;
  ctx.r5.u64 = source.size();
  ctx.r6.u64 = name;
  sub_824C02C0(ctx, base);
  uint32_t status = ctx.r3.u32;
  if (!status) {
    ctx.r3.u64 = L;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = 1;
    ctx.r6.u64 = 0;
    sub_824BEF00(ctx, base);
    status = ctx.r3.u32;
  }
  if (result) {
    ctx.r3.u64 = L;
    ctx.r4.s64 = -1;
    ctx.r5.u64 = 0;
    sub_824BE118(ctx, base);
    if (const uint32_t text = ctx.r3.u32) {
      const char* chars = reinterpret_cast<const char*>(base + text);
      result->assign(chars, strnlen(chars, 8192));
    }
  }
  ctx.r3.u64 = L;
  ctx.r4.u64 = top;
  sub_824BDA38(ctx, base);
  ctx = saved;
  return status == 0;
}

}  // namespace cc1::lua

// Scripts tick at the entry of the GUI update, where no Lua call is in progress.
REX_HOOK_RAW(sub_8296E0F8) {
  cc1::lua::Tick(ctx, base);
  cc1::mouse::Tick(ctx, base);
  __imp__sub_8296E0F8(ctx, base);
}
