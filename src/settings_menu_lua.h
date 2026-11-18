// Lua half of CC1's extended settings page, installed and driven by settings.cpp: hooks RunScript, SetToolTip,
// ApplyMenuOptionFromStack and Transition to turn the three rows into a window onto a longer list, and queues
// events for the host to read through CC1S_Poll().

#pragma once

namespace cc1::settings {

inline constexpr char kMenuScript[] = R"lua(
if rawget(_G, "CC1S") then return "have" end
if not (CC1_Hook and RunScript and SetToolTip and ApplyMenuOptionFromStack and Transition and
        ToUniString) then
  return "wait"
end

local S = { page = nil, top = 1, focus = 0, events = "", host = {}, native = {}, spaced = {} }
rawset(_G, "CC1S", S)

local TIPS = "menus/help_and_options/tooltips/settings/"
local SLOTS = {
  { button = "sfx_button", bar = "sfx_bar", up = "menus/settings/upsound",
    down = "menus/settings/downsound", tip = TIPS .. "sound" },
  { button = "music_button", bar = "music_bar", up = "menus/settings/upmusic",
    down = "menus/settings/downmusic", tip = TIPS .. "music" },
  { button = "rumble_button", bar = "rumble_bar", up = "menus/settings/uprumble",
    down = "menus/settings/downrumble", tip = TIPS .. "rumble" },
}

-- The game's own settings as list entries 1-3 (what its up*/down* scripts do).
local NATIVE = {
  { get = function() return resources.SoundMasterVolume end,
    set = function(v) resources.SoundMasterVolume = v end,
    test = "menus/settings/playsoundtest", tip = SLOTS[1].tip },
  { get = function() return resources.MusicMasterVolume end,
    set = function(v) resources.MusicMasterVolume = v end, tip = SLOTS[2].tip },
  { get = function() return ProfileGetRumbleIntensity() end,
    set = function(v) ProfileSetRumbleIntensity(v) end,
    test = "menus/settings/playrumbletest", tip = SLOTS[3].tip },
}

local BREAK = string.char(13, 10)  -- a line break in a label

local function event(text) S.events = S.events .. text .. ";" end
local function count() return 3 + #S.host end

-- Shows list entries top..top+2 in the three rows.
local function refresh()
  local page = S.page
  if not page then return end
  for slot = 1, 3 do
    local entry = S.top + slot - 1
    local text, fraction, spacing
    if entry <= 3 then
      text, fraction, spacing = S.native[entry], NATIVE[entry].get(), 1
    else
      local h = S.host[entry - 3]
      -- Two lines and a trailing break, as the game's own rows are laid out.
      text, fraction, spacing = ToUniString(h[1] .. BREAK .. h[2] .. BREAK), h[3], h[4] or 1
    end
    local label = page[SLOTS[slot].button].text
    -- Line spacing an entry asks for cannot be set from script, so the host is told and sets it.
    if S.spaced[slot] ~= spacing then
      S.spaced[slot] = spacing
      event("space," .. slot .. "," .. spacing)
    end
    label.Text = text
    page[SLOTS[slot].bar].scrollbar.Value = fraction
  end
end

local function adjust(entry, direction)
  if entry <= 3 then
    local n = NATIVE[entry]
    local v = math.floor((n.get() + direction * 0.1) * 10 + 0.5) / 10
    if v < 0 then v = 0 elseif v > 1 then v = 1 end
    n.set(v)
    refresh()
    if n.test then RunScript(n.test) end
  else
    event("adj," .. (entry - 3) .. "," .. direction)
  end
end

-- Handles a row's script, returning true when it was handled here.
local function on_script(name)
  for slot = 1, 3 do
    if name == SLOTS[slot].up then adjust(S.top + slot - 1, 1) return true end
    if name == SLOTS[slot].down then adjust(S.top + slot - 1, -1) return true end
  end
  if name == "menus/settings/actions/save_settings" then
    event("apply")
  elseif name == "menus/settings/actions/cancel_changes" then
    event("cancel")
  elseif name == "menus/settings/actions/reset_to_defaults" then
    for i = 1, 3 do NATIVE[i].set(0.7) end
    event("reset")
    refresh()
    return true
  end
  return false
end

-- On a row taking focus, turns the page when the cursor wrapped past either end and returns the tooltip for
-- the entry under it.
local function on_tooltip(id)
  local slot
  for i = 1, 3 do if SLOTS[i].tip == id then slot = i end end
  if not slot then return nil end
  local previous = S.focus
  S.focus = slot
  local last = math.max(1, count() - 2)  -- the top of the last page
  if previous == 3 and slot == 1 then  -- down from the last row
    S.top = S.top >= last and 1 or math.min(S.top + 3, last)
    refresh()
  elseif previous == 1 and slot == 3 then  -- up from the first row
    S.top = S.top <= 1 and last or math.max(S.top - 3, 1)
    refresh()
  end
  local entry = S.top + slot - 1
  if entry <= 3 then return NATIVE[entry].tip end
  return "common/empty"
end

-- Copies the game's shared 'me' table, whose object id is rewritten for every script it runs, so the list
-- keeps one that stays on the page.
local function pin(object)
  local copy = {}
  for key, value in pairs(object) do copy[key] = value end
  return setmetatable(copy, getmetatable(object))
end

local function open(page)
  page = pin(page)
  S.page, S.top, S.focus = page, 1, 0
  for i = 1, 3 do S.native[i] = page[SLOTS[i].button].text.Text end
  S.spaced = {}
  event("open")
end

local function guarded(fn, ...)
  local ok, result = pcall(fn, ...)
  if ok then return result end
  event("err," .. tostring(result))
  return nil
end

CC1_Hook("RunScript", function(next, name, ...)
  if S.page and type(name) == "string" and guarded(on_script, name) then return end
  return next(name, ...)
end)
CC1_Hook("SetToolTip", function(next, id, ...)
  if S.page and type(id) == "string" then
    local replacement = guarded(on_tooltip, id)
    if replacement then return next(replacement, ...) end
  end
  return next(id, ...)
end)
CC1_Hook("ApplyMenuOptionFromStack", function(next, page, default, ...)
  if default == "sfx_button" and page then guarded(open, page) end
  return next(page, default, ...)
end)
CC1_Hook("Transition", function(next, ...)
  if S.page then
    S.page = nil
    event("close")
  end
  return next(...)
end)

-- Host call once per GUI frame, returning the queued events.
rawset(_G, "CC1S_Poll", function()
  local events = S.events
  S.events = ""
  return events
end)

-- Host call for the mouse wheel: scrolls 'steps' entries down the list.
rawset(_G, "CC1S_Scroll", function(steps)
  if not S.page then return end
  local top = math.max(1, math.min(S.top + steps, math.max(1, count() - 2)))
  if top ~= S.top then
    S.top = top
    guarded(refresh)
  end
end)

-- Host call while the pointer holds the bar of row 'slot' at 'fraction' of its length: sets one of the game's
-- own settings here, or returns the host's entry (0 otherwise) for the host to set.
rawset(_G, "CC1S_Drag", function(slot, fraction)
  if not S.page then return 0 end
  local entry = S.top + slot - 1
  if entry > count() then return 0 end
  if entry > 3 then return entry - 3 end
  local n = NATIVE[entry]
  local v = math.floor(fraction * 10 + 0.5) / 10
  if math.abs(v - n.get()) > 0.01 then
    n.set(v)
    guarded(refresh)
    if n.test then RunScript(n.test) end
  end
  return 0
end)

-- Host call setting its entries, each { first line, second line, bar fraction, line spacing factor or nil }.
rawset(_G, "CC1S_Set", function(entries)
  S.host = entries
  guarded(refresh)
end)

return "installed"
)lua";

}  // namespace cc1::settings
