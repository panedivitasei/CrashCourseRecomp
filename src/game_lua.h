// game_lua.h: runs registered Lua scripts on the game's GUI Lua state, once per GUI update
// before the game steps its GUI groups.

#pragma once

#include <string>
#include <string_view>

namespace cc1::lua {

struct Script {
  // For log lines.
  const char* name;
  // Runs once per Lua state, returning "wait" to be retried next frame until the game's
  // own globals are defined.
  const char* install;
  // Runs every frame once installed, with its result passed to on_events and a failure
  // causing a reinstall.
  const char* poll;
  // Called after a successful install.
  void (*on_installed)();
  // Called every frame with the poll's result (often empty).
  void (*on_events)(const std::string& events);
};

// Registers a script before the game starts; the pointers must stay valid.
void Register(const Script& script);

// Compiles and runs 'source' on the game's state with the chunk's first result or error
// message in 'result', returning false on an error or outside a script's turn.
bool Run(std::string_view source, std::string* result = nullptr);

// A warning for the log, rate-limited across all scripts.
void LogError(const char* what, const std::string& message);

}  // namespace cc1::lua
