// Lua half of watching a match, installed and driven by spectator.cpp: labels the lobby's X prompt, marks the boxes
// and result rows of watching players, names the watched racer on the HUD panel and trims the map's bars.

#pragma once

namespace cc1::spectator {

inline constexpr char kScript[] = R"lua(
if rawget(_G, "CC1SP") then return "have" end
if not (CC1_Hook and EnableButton and DisableButton and ToUniString and ToString and Exist and CreateV2) then
  return "wait"
end

local P = { label = nil, names = {}, marks = false, frames = 0, events = "", told = {}, home = {} }
local RAISED = 0.045  -- a box's mark stands this far above the gamertag it would cover
rawset(_G, "CC1SP", P)

local function in_lobby() return gui.InLobby and Exist("gui.guiScreens.options.boxes") end
local function lobby()
  local ok, shown = pcall(in_lobby)
  return ok and shown and true or false
end

-- Runs 'fn' and reports a failure to the host once.
local function guarded(fn)
  local ok, result = pcall(fn)
  if ok then return result end
  result = tostring(result)
  if not P.told[result] then
    P.told[result] = true
    P.events = P.events .. "err," .. string.gsub(result, ";", ",") .. ";"
  end
  return nil
end

local function label_x() gui.gui.background.actions.button_x_text.Text = ToUniString(P.label) end

-- In a lobby with spectator seats the X prompt carries the host's label or is hidden, and no Gamer Card prompt is
-- offered anywhere else.
CC1_Hook("EnableButton", function(next, button, label, ...)
  if button == "x" and P.marks and lobby() then
    if not P.label then return DisableButton("x") end
    local result = next(button, label, ...)
    pcall(label_x)
    return result
  end
  if label == "common/actions/gamercard" then return DisableButton(button) end
  return next(button, label, ...)
end)

-- Re-issues the prompt when the host's label changes.
local function prompt()
  if P.marks and g_inTransition ~= 1 then EnableButton("x", "common/actions/gamercard") end
end

local function mark()
  local boxes = gui.guiScreens.options.boxes
  for slot = 0, 3 do
    local text = ""
    if netman.IsSlotUsed(slot) and P.names[string.lower(ToString(netman.PlayerNames[slot]))] then
      text = "Spectator"
    end
    local label = boxes["join" .. slot].text
    if not P.home[slot] then
      local at = label.RelScreenPos
      P.home[slot] = { x = at.x, y = at.y }
    end
    local home = P.home[slot]
    label.RelScreenPos = CreateV2(home.x, text ~= "" and home.y - RAISED or home.y)
    label.Text = ToUniString(text)
  end
end

local function panel()
  gui.hud["p" .. game.GetLocalSlot() .. "hud"].gamertag.Text = ToUniString(P.watch)
end

local function bars()
  for bar = P.bars, 3 do
    local path = "gui.hud.minimap.p" .. bar .. "bar"
    if Exist(path) then gui.hud.minimap["p" .. bar .. "bar"].Visible = false end
  end
end

local function results()
  local rows = gui.hud.tally.tally_mp
  for place = 0, 3 do
    local row = rows["player" .. place]
    if row and P.names[string.lower(ToString(row.gamertag.Text))] then
      row.msg_bar.Text = ToUniString("Spectator")
      row.time.Text = ToUniString("")
    end
  end
end

-- Host call: the X prompt's label (nil for the game's own), the names of the watching players, whether boxes are
-- marked, the racer being watched (nil for none) and how many map bars to keep (nil for all).
rawset(_G, "CC1SP_Set", function(label, names, marks, watch, kept)
  P.label = label
  P.watch = watch
  P.bars = kept
  P.names = {}
  for _, name in ipairs(names) do P.names[string.lower(name)] = true end
  if lobby() and (marks or P.marks) and g_inTransition ~= 1 then guarded(mark) end
  P.marks = marks
  if lobby() then guarded(prompt) end
end)

-- Host call once per GUI frame, re-marking the boxes because they go by slot and slots change hands.
rawset(_G, "CC1SP_Poll", function()
  P.frames = P.frames + 1
  local shown = lobby()
  if not shown then P.home = {} end  -- the next lobby is a new screen
  if P.marks and P.frames % 10 == 0 then
    if shown and g_inTransition ~= 1 then
      guarded(mark)
      if P.frames % 60 == 0 then guarded(prompt) end
    elseif Exist("gui.hud.tally.tally_mp") then
      guarded(results)
    end
  end
  if P.frames % 10 == 0 and (P.watch or P.bars) and Exist("gui.hud") then
    if P.watch then guarded(panel) end
    if P.bars then guarded(bars) end
  end
  local events = P.events
  P.events = ""
  return shown and events .. "lobby;" or events
end)

return "installed"
)lua";

}  // namespace cc1::spectator
