// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include <OptionParser.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

#include "Common/FileUtil.h"
#include "Common/ScopeGuard.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/Host.h"
#include "Core/Movie.h"
#include "Core/System.h"

#include "UICommon/CommandLineParse.h"
#ifdef USE_DISCORD_PRESENCE
#include "UICommon/DiscordPresence.h"
#endif
#include "UICommon/UICommon.h"

static std::unique_ptr<Platform> s_platform;

static void signal_handler(int)
{
  constexpr char message[] = "A signal was received. A second signal will force Dolphin to stop.\n";
#ifdef _WIN32
  puts(message);
#else
  if (write(STDERR_FILENO, message, sizeof(message)) < 0)
  {
  }
#endif

  s_platform->RequestShutdown();
}

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}

void Host_PPCSymbolsChanged()
{
}

void Host_PPCBreakpointsChanged()
{
}

bool Host_UIBlocksControllerState()
{
  return false;
}

void Host_Message(const HostMessageID id)
{
  if (id == HostMessageID::WMUserStop)
    s_platform->Stop();
}

void Host_UpdateTitle(const std::string& title)
{
  s_platform->SetTitle(title);
}

void Host_UpdateDisasmDialog()
{
}

void Host_JitCacheInvalidation()
{
}

void Host_JitProfileDataWiped()
{
}

void Host_RequestRenderWindowSize(int width, int height)
{
}

bool Host_RendererHasFocus()
{
  return s_platform->IsWindowFocused();
}

bool Host_RendererHasFullFocus()
{
  // Mouse capturing isn't implemented
  return Host_RendererHasFocus();
}

bool Host_RendererIsFullscreen()
{
  return s_platform->IsWindowFullscreen();
}

bool Host_TASInputHasFocus()
{
  return false;
}

void Host_YieldToUI()
{
}

void Host_TitleChanged()
{
#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif
}

void Host_UpdateDiscordClientID(const std::string& client_id)
{
#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateClientID(client_id);
#endif
}

bool Host_UpdateDiscordPresenceRaw(const std::string& details, const std::string& state,
                                   const std::string& large_image_key,
                                   const std::string& large_image_text,
                                   const std::string& small_image_key,
                                   const std::string& small_image_text,
                                   const int64_t start_timestamp, const int64_t end_timestamp,
                                   const int party_size, const int party_max)
{
#ifdef USE_DISCORD_PRESENCE
  return Discord::UpdateDiscordPresenceRaw(details, state, large_image_key, large_image_text,
                                           small_image_key, small_image_text, start_timestamp,
                                           end_timestamp, party_size, party_max);
#else
  return false;
#endif
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core)
{
  return nullptr;
}

static std::unique_ptr<Platform> GetPlatform(const optparse::Values& options)
{
  std::string platform_name = static_cast<const char*>(options.get("platform"));

#if HAVE_X11
  if (platform_name == "x11" || platform_name.empty())
    return Platform::CreateX11Platform();
#endif

#ifdef __linux__
  if (platform_name == "fbdev" || platform_name.empty())
    return Platform::CreateFBDevPlatform();
#endif

#ifdef _WIN32
  if (platform_name == "win32" || platform_name.empty())
    return Platform::CreateWin32Platform();
#endif
#ifdef __APPLE__
  if (platform_name == "macos" || platform_name.empty())
    return Platform::CreateMacOSPlatform();
#endif

  if (platform_name == "headless" || platform_name.empty())
    return Platform::CreateHeadlessPlatform();

  return nullptr;
}

#ifdef _WIN32
#define main app_main
#endif

int main(const int argc, char* argv[])
{
  const auto parser =
      CommandLineParse::CreateParser(CommandLineParse::ParserOptions::OmitGUIOptions);
  parser->add_option("--watch")
      .action("store")
      .help("Print each movie input poll where hex ADDRESS:SIZE=VALUE starts to hold");
  parser->add_option("--splice")
      .action("append")
      .help("N:FILE: after the Nth watch match, take movie inputs from FILE");
  parser->add_option("--dump-from-frame")
      .action("store")
      .help("Start dumping frames after this many emulated frames");
  parser->add_option("--dump-after-watch")
      .action("store")
      .help("Start dumping frames after the Nth watch match");
  parser->add_option("--stop-frame").action("store").help("Stop after this many emulated frames");
  parser->add_option("--stop-after-splices")
      .action("store_true")
      .help("Stop once every splice has been played");
  parser->add_option("-p", "--platform")
      .action("store")
      .help("Window platform to use [%choices]")
      .choices({"headless"
#ifdef __linux__
                ,
                "fbdev"
#endif
#if HAVE_X11
                ,
                "x11"
#endif
#ifdef _WIN32
                ,
                "win32"
#endif
#ifdef __APPLE__
                ,
                "macos"
#endif
      });

  optparse::Values& options = CommandLineParse::ParseArguments(parser.get(), argc, argv);
  std::vector<std::string> args = parser->args();

  std::optional<std::string> save_state_path;
  if (options.is_set("save_state"))
  {
    save_state_path = static_cast<const char*>(options.get("save_state"));
  }

  std::unique_ptr<BootParameters> boot;
  bool game_specified = false;
  if (options.is_set("exec"))
  {
    const std::list<std::string> paths_list = options.all("exec");
    const std::vector<std::string> paths{std::make_move_iterator(std::begin(paths_list)),
                                         std::make_move_iterator(std::end(paths_list))};
    boot = BootParameters::GenerateFromFile(
        paths, BootSessionData(save_state_path, DeleteSavestateAfterBoot::No));
    game_specified = true;
  }
  else if (options.is_set("nand_title"))
  {
    const std::string hex_string = static_cast<const char*>(options.get("nand_title"));
    if (hex_string.length() != 16)
    {
      fprintf(stderr, "Invalid title ID\n");
      parser->print_help();
      return 1;
    }
    const u64 title_id = std::stoull(hex_string, nullptr, 16);
    boot = std::make_unique<BootParameters>(BootParameters::NANDTitle{title_id});
  }
  else if (args.size())
  {
    boot = BootParameters::GenerateFromFile(
        args.front(), BootSessionData(save_state_path, DeleteSavestateAfterBoot::No));
    args.erase(args.begin());
    game_specified = true;
  }
  else
  {
    parser->print_help();
    return 0;
  }

  std::string user_directory;
  if (options.is_set("user"))
    user_directory = static_cast<const char*>(options.get("user"));

  s_platform = GetPlatform(options);
  if (!s_platform || !s_platform->Init())
  {
    fprintf(stderr, "No platform found, or failed to initialize.\n");
    return 1;
  }

  const WindowSystemInfo wsi = s_platform->GetWindowSystemInfo();

  UICommon::SetUserDirectory(user_directory);
  UICommon::Init();
  UICommon::InitControllers(wsi);

  Common::ScopeGuard ui_common_guard([] {
    UICommon::ShutdownControllers();
    UICommon::Shutdown();
  });

  if (save_state_path && !game_specified)
  {
    fprintf(stderr, "A save state cannot be loaded without specifying a game to launch.\n");
    return 1;
  }

  auto core_state_changed_hook = Core::AddOnStateChangedCallback([](const Core::State state) {
    if (state == Core::State::Uninitialized)
      s_platform->Stop();
  });

#ifdef _WIN32
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
#else
  // Shut down cleanly on SIGINT and SIGTERM
  struct sigaction sa;
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_RESETHAND;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
#endif

  DolphinAnalytics::Instance().ReportDolphinStart("nogui");

  auto& movie = Core::System::GetInstance().GetMovie();
  if (options.is_set("watch"))
  {
    unsigned address = 0, size = 0, value = 0;
    const std::string watch = static_cast<const char*>(options.get("watch"));
    if (std::sscanf(watch.c_str(), "%x:%u=%u", &address, &size, &value) != 3 ||
        (size != 1 && size != 2 && size != 4))
    {
      fprintf(stderr, "--watch needs ADDRESS:SIZE=VALUE with SIZE 1, 2 or 4\n");
      return 1;
    }
    movie.SetWatch(address, size, value);
  }
  for (const std::string& splice : options.all("splice"))
  {
    const size_t colon = splice.find(':');
    std::string data;
    if (colon == std::string::npos ||
        !File::ReadFileToString(splice.substr(colon + 1), data) || data.size() % 8 != 0)
    {
      fprintf(stderr, "--splice needs N:FILE with a whole number of 8-byte inputs\n");
      return 1;
    }
    movie.AddSplice(static_cast<u32>(std::strtoul(splice.c_str(), nullptr, 10)),
                    std::vector<u8>(data.begin(), data.end()));
  }
  if (options.is_set("dump_from_frame"))
    movie.SetDumpFrame(std::strtoull(static_cast<const char*>(options.get("dump_from_frame")), nullptr, 10));
  if (options.is_set("dump_after_watch"))
    movie.SetDumpAfterWatch(
        static_cast<u32>(std::strtoul(static_cast<const char*>(options.get("dump_after_watch")), nullptr, 10)));
  if (options.is_set("stop_frame"))
    movie.SetStopFrame(std::strtoull(static_cast<const char*>(options.get("stop_frame")), nullptr, 10));
  movie.SetStopAfterSplices(options.is_set("stop_after_splices"));
  if (options.is_set("movie"))
  {
    std::optional<std::string> movie_state;
    if (!movie.PlayInput(static_cast<const char*>(options.get("movie")), &movie_state))
    {
      fprintf(stderr, "Could not play the movie\n");
      return 1;
    }
    if (movie_state)
    {
      fprintf(stderr, "Movies starting from a save state are not supported\n");
      return 1;
    }
  }

  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot), wsi))
  {
    fprintf(stderr, "Could not boot the specified file\n");
    return 1;
  }

#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif

  s_platform->MainLoop();
  Core::Stop(Core::System::GetInstance());

  Core::Shutdown(Core::System::GetInstance());
  s_platform.reset();

  return 0;
}

#ifdef _WIN32
int wmain(int, wchar_t*[], wchar_t*[])
{
  std::vector<std::string> args = Common::CommandLineToUtf8Argv(GetCommandLineW());
  const int argc = static_cast<int>(args.size());
  std::vector<char*> argv(args.size());
  for (size_t i = 0; i < args.size(); ++i)
    argv[i] = args[i].data();

  return main(argc, argv.data());
}

#undef main
#endif
