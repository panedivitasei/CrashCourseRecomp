// intro_skip.cpp: lets A skip the launch logos by making IsDev() answer true to the scripts of the logos/logos GUI
// group, identified by its xbla child.

#include "intro_skip.h"

#include <string>

#include "game_lua.h"

namespace cc1 {
namespace {

constexpr char kScript[] = R"lua(
if rawget(_G, "CC1I") then return "have" end
if not (CC1_Hook and IsDev) then return "wait" end
rawset(_G, "CC1I", true)

CC1_Hook("IsDev", function(next, ...)
  local ok, logos = pcall(function() return me.xbla ~= nil end)
  if ok and logos then return true end
  return next(...)
end)

return "installed"
)lua";

void OnEvents(const std::string&) {}

}  // namespace

void InitializeIntroSkip() { lua::Register({"intro", kScript, "return ''", nullptr, OnEvents}); }

}  // namespace cc1
