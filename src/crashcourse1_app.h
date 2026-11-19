// crashcourse1_app.h: the rex::ReXApp subclass of Crash Course 1.

#pragma once

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/achievement_notification.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#include <jmlive/jmlive.h>
#include <rexglue_guide/guide.h>

#include <overlay.h>  // src/gpu: the ImGui overlay on the native renderer

#include "content_seed.h"
#include "intro_skip.h"
#include "input_assignment.h"
#include "invite.h"
#include "controls.h"
#include "local_players.h"
#include "mouse.h"
#include "online.h"
#include "spectator.h"
#include "settings.h"
#include "version.h"

REXCVAR_DEFINE_STRING(cc1_server_url, "https://cc1-server-yb4os3fhiq-ue.a.run.app", "CC1",
                      "The JMstudios CC1 server: Game Jolt sign-ins, friends for the Guide, and lobbies.");
REXCVAR_DEFINE_STRING(cc1_invite_link, "https://gamejolt.com/games/crashcourserecompiled/1118129", "CC1",
                      "What Invite a Friend in the online lobby shows and copies: the game's page.");
REXCVAR_DEFINE_BOOL(cc1_online, true, "CC1",
                    "Off keeps the Guide on the local profile: no Game Jolt sign-in and no network requests.");
REXCVAR_DEFINE_STRING(dump_image_path, "", "CC1",
                      "Writes the decrypted, patched guest image to this path and exits before the game starts.");

// Native renderer cvars (src/gpu/video_native.cpp).
REXCVAR_DEFINE_BOOL(cc1_dev_keys, false, "CC1",
                    "Keeps the runtime's overlay keys: the console, the debug overlay, its settings and "
                    "achievements.");
REXCVAR_DECLARE(bool, gpu_native);
REXCVAR_DECLARE(bool, gpu_xenia_gpu);

class Crashcourse1App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Crashcourse1App>(new Crashcourse1App(ctx, "crashcourse1",
        PPCImageConfig));
  }

  // Loads crashcourse1.toml before the runtime fills the paths from cvars, and resolves
  // relative paths against the executable's folder.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (std::filesystem::exists(paths.config_path)) rex::cvar::LoadConfig(paths.config_path);
    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    // An installed game (user_data_root set) keeps one log beside the exe, restarted at each launch and flushed every
    // second.
    if (!rex::cvar::Query<std::string>("user_data_root").empty()) {
      if (rex::cvar::Query<std::string>("log_file").empty()) {
        rex::cvar::SetFlagByName("log_file", (exe_dir / "crashcourse1.log").string());
      }
      if (rex::cvar::GetFlagSource("log_flush_interval") == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName("log_flush_interval", "1");
      }
      const std::filesystem::path log = rex::cvar::Query<std::string>("log_file");
      std::error_code ec;
      std::filesystem::remove(log, ec);
      std::filesystem::remove(std::filesystem::path(log).replace_extension(".1.log"), ec);
    }
    auto folder = [&](const char* cvar, std::filesystem::path& out) {
      const std::string value = rex::cvar::Query<std::string>(cvar);
      if (value.empty()) return false;
      std::filesystem::path p = value;
      out = (p.is_relative() ? exe_dir / p : p).lexically_normal();
      return true;
    };
    // The game starts windowed until the player chooses fullscreen in Settings; the runtime's own default is fullscreen.
    if (rex::cvar::GetFlagSource("fullscreen") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("fullscreen", "false");
    }
    folder("game_data_root", paths.game_data_root);
    folder("update_data_root", paths.update_data_root);
    // Saves and profiles default to the shared ReXGlue user folder unless the config names one.
    if (!folder("user_data_root", paths.user_data_root)) {
      paths.user_data_root = rex::filesystem::GetUserFolder() / "ReXGlue" / "userdata";
    }
    if (!folder("cache_root", paths.cache_root)) paths.cache_root = paths.user_data_root / "cache";
  }

  bool SetupPresentation() override {
    if (!rex::ReXApp::SetupPresentation()) return false;
    window()->SetTitle("Crash Course 1");
    cc1::settings::Initialize(window());
    cc1::online::Initialize(REXCVAR_GET(cc1_server_url));
    cc1::spectator::Initialize();
    cc1::InitializeLocalPlayers();
    cc1::InitializeIntroSkip();
    cc1::mouse::Initialize(window());
    cc1::controls::Initialize(window());
    cc1::invite::Initialize(REXCVAR_GET(cc1_invite_link));
    // The renderer requests one UI frame per present, drawn on the UI thread.
    rex::videonative::overlay::SetFrameCallback([this] {
      if (overlay_queued_.exchange(true)) return;
      if (!app_context().CallInUIThread([this] {
            overlay_queued_ = false;
            DrawOverlay();
          })) {
        overlay_queued_ = false;
      }
    });
    return true;
  }

  // Detached mode (no SDK graphics system): ImGui draws through the native renderer.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    return rex::videonative::overlay::CreateDrawer();
  }

  void OnConfigureFonts(ImFontAtlas* atlas) override {
    rex::ReXApp::OnConfigureFonts(atlas);
    rexglue_guide::ConfigureFonts(atlas);
  }

  // JMlive is the Guide on the local profile with Game Jolt sign-in, its data shared with
  // the other JMstudios titles.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // The runtime has bound its overlays to the backtick, F3, F4 and F7 by now.
    if (!REXCVAR_GET(cc1_dev_keys)) {
      for (const char* bind : {"bind_console", "bind_debug_overlay", "bind_settings", "bind_achievements"}) {
        rex::ui::UnregisterBind(bind);
      }
    }
    const std::filesystem::path jmlive_dir =
        rex::filesystem::GetUserFolder() / "JMstudios" / "userdata" / "JMlive";
    jmlive::Configure({.gamertag = rex::cvar::Query<std::string>("user_name"),
                       .server_url = REXCVAR_GET(cc1_server_url),
                       .client_version = CC1_VERSION_TEXT,
                       .online = REXCVAR_GET(cc1_online)});
    rexglue_guide::Install(this, drawer,
                           {.title_name = "Crash Course",
                            .data_dir = jmlive_dir,
                            .on_quit =
                                [this] {
                                  if (window()) window()->RequestClose();
                                },
                            .immediate_drawer = immediate_drawer(),
                            .config_file = rex::filesystem::GetExecutableFolder() /
                                           "crashcourse1.toml",
                            // The two Xbox LIVE achievements are earned in a lobby match (achievements.cpp).
                            .achievement_text = [](uint32_t id) {
                              return std::string(id == 2   ? "Beat a friend in an online race"
                                                 : id == 7 ? "Finished every level online"
                                                           : "");
                            }});
  }

  // The Guide has its own unlock toasts and achievements page.
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog()
      override {
    return nullptr;
  }
  std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override { return nullptr; }

  void OnShutdown() override {
    cc1::online::Shutdown();
    cc1::controls::Shutdown();
    cc1::mouse::Shutdown();
    cc1::settings::Shutdown();
    rex::videonative::overlay::SetFrameCallback(nullptr);
    rexglue_guide::Shutdown();
  }

  // UI thread: records one ImGui frame for the renderer's next present.
  void DrawOverlay() {
    if (!imgui_drawer() || !window()) return;
    rex::ui::AppUIDrawContext context(window()->GetActualPhysicalWidth(),
                                      window()->GetActualPhysicalHeight());
    rex::videonative::overlay::BeginFrame();
    imgui_drawer()->Draw(context);
    rex::videonative::overlay::EndFrame();
  }

  // Drops the Xenia graphics system the runtime set up unless gpu_xenia_gpu asks
  // for it, since the native renderer (src/gpu) draws the guest's D3D calls on its own device.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.kernel_init = cc1::online::InitializeKernel;  // the runtime's kernel init plus this title's LIVE apps
    if (REXCVAR_GET(gpu_native) && !REXCVAR_GET(gpu_xenia_gpu)) {
      REXLOG_INFO("crashcourse1: gpu_native set - Xenia graphics system not created");
      config.graphics.reset();
    }
    // Identity settings shared by every ReXGlue title (user_name, user_gamerpic) load after
    // the game toml so they win.
    const auto shared = user_data_root() / "user.toml";
    if (std::filesystem::exists(shared)) rex::cvar::LoadConfig(shared);
  }

  void OnPostSetup() override {
    // The game is the full one whatever the config says: the runtime's mask is 0, the trial.
    rex::cvar::SetFlagByName("license_mask", "2147483647");
    // An installed game's log keeps the kernel's errors and the renderer's warnings, unless the config sets a level.
    if (!rex::cvar::Query<std::string>("user_data_root").empty() &&
        rex::cvar::GetFlagSource("log_level") == rex::cvar::Source::kDefault) {
      rex::SetCategoryLevel(rex::log::krnl(), spdlog::level::err);
      rex::SetCategoryLevel(rex::log::gpu(), spdlog::level::warn);
    }
    cc1::SeedContent(runtime()->game_data_root(), runtime()->user_data_root());
    cc1::InstallInputAssignment(static_cast<rex::input::InputSystem*>(runtime()->input_system()));
  }

  // tools/shaderpack reads the engine's built-in shaders out of this image.
  void OnPostLoadXexImage() override {
    const std::string path = REXCVAR_GET(dump_image_path);
    if (path.empty()) return;
    const auto* image = runtime()->memory()->TranslateVirtual<const char*>(PPCImageConfig.image_base);
    bool written = false;
    if (image) {
      std::ofstream file(path, std::ios::binary | std::ios::trunc);
      written = static_cast<bool>(file.write(image, PPCImageConfig.image_size).flush());
    }
    if (written) {
      REXLOG_INFO("crashcourse1: wrote the guest image to {} ({} bytes)", path,
                  uint32_t(PPCImageConfig.image_size));
    } else {
      REXLOG_ERROR("crashcourse1: could not write the guest image to {}", path);
    }
    rex::FlushLogging();
    // A quit through the UI loop faults in shutdown when no module has launched.
    std::_Exit(written ? EXIT_SUCCESS : EXIT_FAILURE);
  }

 private:
  std::atomic<bool> overlay_queued_{false};
};
