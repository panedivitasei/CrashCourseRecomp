// invite.cpp: the Invite a Friend window (see invite.h).

#include "invite.h"

#include <atomic>
#include <cstring>
#include <string>
#include <utility>

#include <rex/hook.h>
#include <rex/types.h>

#include <rexglue_guide/guide.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace cc1::invite {
namespace {

using rex::u32;

constexpr char kMenuId[] = "cc1-invite";

std::string g_link;
std::atomic<bool> g_copied{false};

bool CopyToClipboard(const std::string& text) {
  const std::wstring wide(text.begin(), text.end());  // the link is ASCII
  const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
  if (!OpenClipboard(nullptr)) return false;
  bool ok = false;
  if (HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (void* data = GlobalLock(block)) {
      std::memcpy(data, wide.c_str(), bytes);
      GlobalUnlock(block);
      EmptyClipboard();
      ok = SetClipboardData(CF_UNICODETEXT, block) != nullptr;
    }
    if (!ok) GlobalFree(block);
  }
  CloseClipboard();
  return ok;
}

// Runs on the Guide's drawing thread each frame the window is up.
void BuildMenu(rexglue_guide::Menu& menu) {
  menu.Text(g_link);
  if (menu.Button("Copy Link")) g_copied = CopyToClipboard(g_link);
  if (g_copied) menu.Text("Copied.", rexglue_guide::Menu::Tone::kDim);
}

// Opens the link window when the game's Invite a Friend button asks for the console's friends list.
u32 ShowFriendsUI(u32 user) {
  g_copied = false;
  rexglue_guide::ShowMenu(kMenuId);
  return 0;
}

}  // namespace

void Initialize(std::string link) {
  g_link = std::move(link);
  rexglue_guide::AddMenu({.id = kMenuId,
                          .title = "Invite a Friend",
                          .place = rexglue_guide::MenuPlace::kDetached,
                          .window = {.width = 720.0f, .height = 300.0f, .hold_input = true},
                          .build = BuildMenu});
}

}  // namespace cc1::invite

REX_HOOK(__imp__XamShowFriendsUI, cc1::invite::ShowFriendsUI)
