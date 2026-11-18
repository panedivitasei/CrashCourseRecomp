// Lua half of Online Multiplayer, installed and driven by online.cpp: relabels the main menu's online button and
// redirects it to the host's lobby list, runs the host's create-match, Kick, Change Course and Back to Lobby flows,
// and queues events for the host to read through CC1O_Poll().

#pragma once

namespace cc1::online {

inline constexpr char kMenuScript[] = R"lua(
if rawget(_G, "CC1O") then return "have" end
if not (CC1_Hook and SetToolTip and Transition and ToUniString and Exist) then return "wait" end

local O = { focus = false, events = "", shown = false, lobby = false, fresh = 0, frames = 0, slide = nil, seats = 4 }
rawset(_G, "CC1O", O)

local BUTTON = "gui.guiScreens.options.buttons.bt_online"
local TIP = "menus/main/tooltips/online_mp"
local MAIN = "menus/main/screen_options"
local LIVE_MENU = "GGRP:menus/online_multiplayer/screen"  -- Quick, Ranked, Create and Custom Match
local SLOTS = "GGRP:menus/online_multiplayer/create_match_options"  -- Total Slots, Private Slots
-- Two lines, as the button's own label is ("Xbox LIVE" / "Play").
local LABEL = ToUniString("Online" .. string.char(13, 10) .. "Multiplayer")

local function event(name) O.events = O.events .. name .. ";" end
local function to_main(target) return type(target) == "string" and string.find(target, MAIN, 1, true) ~= nil end

CC1_Hook("SetToolTip", function(next, id, ...)
  O.focus = id == TIP
  return next(id, ...)
end)
CC1_Hook("CheckMultiplayerPrivilege", function(next, ...)
  if O.focus then
    event("lobbies")
    return false
  end
  return next(...)
end)
local LOBBY = "GGRP:menus/online_multiplayer/lobby"
-- The country and course screens, where online the course screen is the custom match options.
local COURSE_SCREENS = { ["GGRP:menus/selectcountry/screen"] = true, ["GGRP:menus/selectlevel/screen"] = true,
                         ["GGRP:menus/online_multiplayer/custom_match_options"] = true }

CC1_Hook("Transition", function(next, target, ...)
  -- While the host chooses a course, every way out of the course screens leads to the lobby.
  if O.course then
    if not COURSE_SCREENS[target] then
      -- The country screen's Back to the main menu fades the logo in, so it is faded out again.
      if to_main(target) and Exist("gui.guiScreens.logo") then
        gui.guiScreens.logo.QueueBh2("B2FadeTo", "1,0,0.2")
      end
      O.course = nil
      O.coursed = 0
      target = LOBBY
    end
    O.fresh = 120
    return next(target, ...)
  end
  -- The lobby list stands in for the game's LIVE menu and slots screen, so going back to one goes back to the
  -- main menu.
  if target == LIVE_MENU or target == SLOTS then target = "GGRP:" .. MAIN end
  O.fresh = 120
  if to_main(target) then event("menu") end
  return next(target, ...)
end)
CC1_Hook("ExitGame", function(next, target, ...)
  if to_main(target) then event("menu") end
  return next(target, ...)
end)

-- The leaderboards are all-time boards whose rows are nobody's profile, so the board type and Gamer Card prompts
-- are disabled.
local BOARD_ONLY = { ["common/actions/gamercard"] = true, ["common/actions/weekly"] = true,
                     ["common/actions/weeklynew"] = true }
local function on_boards() return gui.ViewingLeaderboards end
-- The main menu's offers with nothing behind them here: a marketplace, a storage device to choose, the game's
-- recommendations.
local GONE = { ["menus/main/buttons/downloadcontent"] = true, ["menus/main/buttons/salectsavegame"] = true,
               ["menus/main/buttons/recommended"] = true }
CC1_Hook("EnableButton", function(next, button, label, ...)
  -- While the host chooses a course, the courses' A selects one instead of making a match.
  if O.course and button == "a" and label == "common/actions/create_match" then
    local result = next(button, label, ...)
    gui.gui.background.actions.button_a_text.Text = ToUniString("Select Course")
    return result
  end
  if BOARD_ONLY[label] then
    local ok, boards = pcall(on_boards)
    if ok and boards then return DisableButton(button) end
  end
  if GONE[label] then
    -- gui's functions are found through its metatable, so one set on the table itself is found first.
    rawset(gui, "SaveSelect", function() end)
    return DisableButton(button)
  end
  return next(button, label, ...)
end)
-- The main menu binds its buttons whether their prompts show or not, so the right trigger's marketplace, the left's
-- gui.SaveSelect (above) and X's recommendations screen are stubbed.
CC1_Hook("ShowMarketplace", function() end)
CC1_Hook("Transition", function(next, target, ...)
  if target == "GGRP:menus/cross_selling/screen" then return end
  return next(target, ...)
end)
-- Level select's rows are speedrun.com's too, so its Y (Gamer Card) is never offered.
CC1_Hook("UpdateYLeaderboardButton", function() end)

local function relabel() gui.guiScreens.options.buttons.bt_online.text.Text = LABEL end

-- The online lobby's heading is the label menus/online_multiplayer/title ("Xbox LIVE"), which no other screen has,
-- replaced here by the name of the server's lobby the match belongs to.
local HEADING = ToUniString("JM LIVE")
local function in_lobby() return gui.InLobby and gui.InLiveMenus and Exist("gui.guiScreens.options.title") end
local function reheading() gui.guiScreens.options.title.Text = O.heading or HEADING end

-- How far each of the results' buttons moves to make room for Y between X and A.
local SPACED = { x = -0.034, y = -0.042, a = 0.011, b = 0.010 }

-- The results' bars (one player's and a match's) have an X of their own for the Gamer Card, hidden because no card
-- is shown.
local function results_x()
  for _, name in ipairs({ "tally_mp", "tally_1p" }) do
    if Exist("gui.hud.tally." .. name) then
      local actions = gui.hud.tally[name].actions
      actions.button_x.Visible = 0
      actions.button_x_text.Visible = 0
    end
  end
end

-- The results' Y is the host's way back to the lobby and the others' line saying whom they wait for; returns true
-- for the host.
local function results_y()
  if not Exist("gui.hud.tally.tally_mp") then
    O.spaced = false  -- the next results are a new screen
    O.over = nil
    return false
  end
  if not O.heading or game.GetGameStateStr() ~= "GAMEOVER" then return false end
  -- TallyScreenButtonsReady never comes for a host with no next course to offer, so the host's Y waits for
  -- DisplayNextLevelButton or three seconds instead.
  O.over = (O.over or 0) + 5
  if netman.IsHost() and not (TallyScreenButtonsReady or game.DisplayNextLevelButton or O.over > 180) then
    return false
  end
  local actions = gui.hud.tally.tally_mp.actions
  if not O.spaced then
    O.spaced = true
    for button, by in pairs(SPACED) do
      for _, name in ipairs({ "button_" .. button, "button_" .. button .. "_text" }) do
        local at = actions[name].RelScreenPos
        actions[name].RelScreenPos = CreateV2(at.x + by, at.y)
      end
    end
  end
  local host = netman.IsHost() and true or false
  actions.button_y.Visible = host and 1 or 0
  actions.button_y_text.Visible = 1
  actions.button_y_text.Text = ToUniString(host and "Back to Lobby" or "Waiting for host...")
  return host
end

-- Host call: the lobby's name (nil when in none) and this player's.
rawset(_G, "CC1O_Heading", function(name, me)
  O.heading = name and ToUniString(name) or nil
  O.me = me and string.lower(me) or nil
  O.lobby = false  -- the heading is set again on the next poll
end)

-- Host call: returns from the results to the lobby with the match and its players as they are.
rawset(_G, "CC1O_Lobby", function()
  local state = tostring(netman.State)
  if Exist("game") then QuitGame() end
  gui.LoadMenu("menus/online_multiplayer/lobby", true)
  gui.DisplayGroup("menumusic", "menus/menu_music_online_mp")
  O.relobby = 0
  return state
end)

local function lobby_up() return gui.InLobby and Exist("gui.guiScreens.options.boxes") end

-- With the lobby up again, sets what the menus it came through would have set and asks for the avatars.
local function relobby()
  O.relobby = O.relobby + 1
  local ok, up = pcall(lobby_up)
  if O.relobby > 10 and ok and up then
    gui.InLiveMenus = true
    LevelInfoLeaderboardType = "localleaderbaord"
    g_stackMenu = g_stackMenu or {}
    O.relobby = nil
    event("relobby")
  elseif O.relobby > 600 then
    O.relobby = nil
  end
end

-- The slot whose box has the focus and a player in it who is not this one; nil when there is none.
local function kickable()
  if not (O.heading and O.me and netman.IsHost()) then return nil end
  local boxes = gui.guiScreens.options.boxes
  for slot = 0, 3 do
    if netman.IsSlotUsed(slot) and boxes["join" .. slot].HasFocus() then
      local name = ToString(netman.PlayerNames[slot])
      if string.lower(name) ~= O.me then return slot, name end
      return nil
    end
  end
  return nil
end

-- The host's Kick prompt, with the name once it has been pressed.
local function kick_prompt()
  local slot, name = kickable()
  if O.kick then
    O.kick.frames = O.kick.frames - 5
    if O.kick.frames <= 0 or O.kick.slot ~= slot then O.kick = nil end
  end
  if slot then
    EnableButton("rt", "common/actions/invite_friend")
    gui.gui.background.actions.button_rt_text.Text = ToUniString(O.kick and ("Kick " .. name .. "?") or "Kick")
  elseif O.kickable then
    DisableButton("rt")
  end
  return slot ~= nil
end

local function no_logo()
  local logo = gui.guiScreens.logo
  if logo.RelOpacity ~= 0 then logo.RelOpacity = 0 end
end

-- The lobby's picture and name of the course, as join_boxes fills them when it comes up.
local function show_course()
  local boxes = gui.guiScreens.options.boxes
  gui.SetLvlConfGuiTrack(gui.GetLvlConfTrack())
  local theme = gui.GetLvlConfThemeStr()
  boxes.level.ReplaceControl("pic", "GGRP:" .. gui.GetLvlConfThumbnail())
  boxes.level.ReplaceControl("bg", theme == "USA" and "GPCT:select_level/usa_bg"
    or theme == "EU" and "GPCT:select_level/europe_bg" or "GPCT:select_level/japan_bg")
  boxes.levelInfo.Text = GetUnistring(gui.GetLvlConfTrackName())
  boxes.shuffleinfo.Text = GetUnistring(gui.GetLvlConfShuffle() and "menus/selectlevel/shuffle"
    or "common/countries/continuous")
end

-- Shows the course again when the level configuration changes, as a member's does when the host's game sends it.
local function follow_course()
  local track = gui.GetLvlConfTrack()
  if O.track == track then return end
  local first = O.track == nil
  O.track = track
  if not first then show_course() end
end

-- The bar's B has its label 0.2 of a screen from its icon where A, X and Y have about 0.17, so it is kept as close
-- as theirs.
local B_LABEL = 0.171
local function tidy_b()
  local actions = gui.gui.background.actions
  local icon, label = actions.button_b.RelScreenPos, actions.button_b_text.RelScreenPos
  if math.abs(label.x - icon.x - B_LABEL) > 0.002 then
    actions.button_b_text.RelScreenPos = CreateV2(icon.x + B_LABEL, label.y)
  end
end

-- Moves the bar's B to 'icon' and 'text' and returns where it was.
local function place_b(icon, text)
  local actions = gui.gui.background.actions
  local was = { actions.button_b.RelScreenPos, actions.button_b_text.RelScreenPos }
  actions.button_b.RelScreenPos = icon
  actions.button_b_text.RelScreenPos = text
  return { CreateV2(was[1].x, was[1].y), CreateV2(was[2].x, was[2].y) }
end

-- Second half of the way into the course screens, once the lobby has slid out.
local function course_in()
  if Exist("gui.guiScreens.options") then gui.guiScreens.options.Close() end
  DisableButton("start")  -- the lobby had it on to its last frame
  O.b = place_b(CreateV2(0.3403, 0.2795), CreateV2(0.3403 + B_LABEL, 0.277))
  postLevelPage = LOBBY
  CustomMatchOptionFuncOnClick = nil
  PrevCustomMatchOptions = LOBBY
  flagselected = 0
  showshuffle = 1
  customselected = 0
  gui.guiScreens.AddControl("options", "GGRP:menus/selectcountry/screen", CreateV2(0, 0), 1)
  gui.guiScreens.options.QueueBh2("B2Move", "(1.35,0),(0,0)," .. 0.2)
  g_inTransition = 0
end

-- Host call: Change Course was pressed in the lobby.
rawset(_G, "CC1O_Course", function()
  if not (O.lobby and O.heading and netman.IsHost() and g_inTransition ~= 1 and not O.slide) then return "" end
  O.course = true
  g_inTransition = 1
  -- Start is disabled too because the lobby turns it on for its host and the course screens have A where it stands.
  for _, button in ipairs({ "a", "b", "x", "y", "rt", "lt", "start" }) do DisableButton(button) end
  gui.BlockInputTimed(0.41)
  gui.guiScreens.options.QueueBh2("B2Move", "(0,0),(1.35,0)," .. 0.2)
  O.slide = 18
  O.into = course_in
  return "course"
end)

-- Host call: Kick was pressed, where the first press asks and a second within three seconds kicks and returns the
-- player's name, otherwise "".
rawset(_G, "CC1O_Kick", function()
  local slot, name = kickable()
  if not slot then return "" end
  if not (O.kick and O.kick.slot == slot) then
    O.kick = { slot = slot, frames = 180 }
    return ""
  end
  O.kick = nil
  netman.KickPlayer(slot)
  return name
end)

local COUNTRY = "GGRP:menus/selectcountry/screen"
local SLIDE = 0.2  -- seconds, as Transition()

-- What Next on the slots screen leaves for the screens after it.
local function slots_chosen()
  CreateGameTotalSlot = O.seats
  CreateGamePrivateSlot = 0
  MessageScreenNext = "GGRP:menus/online_multiplayer/lobby"
  MessageScreenSuccess = "LOBBY"
  CustomMatchOptionFuncOnClick = function()
    netman.CreateMatch(gui.GetCurrentGamepad(), false, CreateGameTotalSlot, CreateGamePrivateSlot)
  end
  MessageScreenButtonCmd = { b = function() Transition(PrevCustomMatchOptions) end }
  MessageScreenOnFail = function() EnableButton("b", "common/actions/back") end
  postLevelPage = "GGRP:menus/online_multiplayer/messages_screen"
  flagselected = 0
end

-- Second half of Transition(COUNTRY), once the old screen has slid out.
local function slide_in()
  if Exist("gui.guiScreens.options") then gui.guiScreens.options.Close() end
  slots_chosen()
  -- The slots screen this passes over moves the bar's B right, clear of the country screen's A.
  pcall(place_b, CreateV2(0.3403, 0.2795), CreateV2(0.3403 + B_LABEL, 0.277))
  gui.guiScreens.AddControl("options", COUNTRY, CreateV2(0, 0), 1)
  gui.guiScreens.options.QueueBh2("B2Move", "(1.35,0),(0,0)," .. SLIDE)
  g_inTransition = 0
end

-- Host call: enters the create-match screens as menus/online_multiplayer/buttons/create_match does from the LIVE
-- menu and the slots screen after it, with the lobby's seats.
rawset(_G, "CC1O_Host", function(seats)
  O.seats = math.max(2, math.min(tonumber(seats) or 4, 4))
  O.focus = false
  flagselected = 0
  showshuffle = 1
  customselected = 0
  gui.SetLevelCondition(3)
  netman.NextSessionType = "LIVE"
  gui.InLiveMenus = true
  AddMenuOptionToStack("bt_online")
  g_inTransition = 1
  for _, button in ipairs({ "a", "b", "x", "y", "rt", "lt" }) do DisableButton(button) end
  gui.BlockInputTimed(SLIDE * 2 + 0.01)
  if Exist("gui.guiScreens.options") then
    gui.guiScreens.options.QueueBh2("B2Move", "(0,0),(1.35,0)," .. SLIDE)
  end
  O.slide = 18  -- polls until the slide-out is over
  return "hosting"
end)

-- Host call once per GUI frame: keeps the two labels in place and returns the events.
rawset(_G, "CC1O_Poll", function()
  local shown = Exist(BUTTON)
  O.frames = O.frames + 1
  if O.fresh > 0 then O.fresh = O.fresh - 1 end
  if shown and (not O.shown or O.fresh > 0 or O.frames % 30 == 0) then
    shown = pcall(relabel)
  end
  O.shown = shown
  local ok, lobby = pcall(in_lobby)
  lobby = ok and lobby and true or false
  if lobby and (not O.lobby or O.frames % 30 == 0) then pcall(reheading) end
  O.lobby = lobby
  if O.slide then
    O.slide = O.slide - 1
    if O.slide <= 0 then
      O.slide = nil
      local into = O.into or slide_in
      O.into = nil
      if not pcall(into) then
        g_inTransition = 0
        if into == slide_in then event("hostfail") else O.course = nil end
      end
    end
  end
  if O.frames % 5 == 0 then
    pcall(results_x)
    local ok, host = pcall(results_y)
    O.results = ok and host
    local shown = false
    if lobby and g_inTransition ~= 1 then
      ok, shown = pcall(kick_prompt)
      shown = ok and shown
    end
    O.kickable = shown
  end
  if O.results then event("results") end
  if O.kickable then event("kick") end
  if O.frames % 5 == 0 and g_inTransition ~= 1 and Exist("gui.gui.background.actions.button_b_text") then
    pcall(tidy_b)
  end
  -- The logo belongs to the main menu, so it is hidden in the lobby and the course screens reached from it.
  if (lobby or O.course) and O.frames % 5 == 0 then pcall(no_logo) end
  -- The host's Change Course, and everybody's lobby following the course.
  if lobby and g_inTransition ~= 1 and O.frames % 15 == 0 then
    pcall(follow_course)
    local ok, host = pcall(netman.IsHost)
    O.hosting = ok and host and O.heading ~= nil
    if O.hosting then
      EnableButton("lt", "common/actions/invite_friend")
      gui.gui.background.actions.button_lt_text.Text = ToUniString("Change Course")
    end
  elseif not lobby then
    O.hosting = false
    O.track = nil
  end
  if O.hosting then event("hosting") end
  if O.coursed then
    O.coursed = O.coursed + 1
    if lobby and O.coursed > 40 and g_inTransition ~= 1 then
      O.coursed = nil
      if O.b then
        pcall(place_b, O.b[1], O.b[2])
        O.b = nil
      end
      EnableButton("x", "common/actions/gamercard")  -- the course screens' way out takes X off the bar
      event("course")
      event("relobby")  -- the lobby is a new screen: its avatars are to be loaded again
    elseif O.coursed > 600 then
      O.coursed = nil
    end
  end
  if O.relobby then relobby() end
  local events = O.events
  O.events = ""
  return events
end)

return "installed"
)lua";

}  // namespace cc1::online
