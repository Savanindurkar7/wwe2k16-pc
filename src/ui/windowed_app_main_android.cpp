#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <algorithm>
#include <spdlog/common.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_android.h>

extern "C" int main(int argc, char** argv) {
  auto init_result = rex::cvar::Init(argc, argv);
  if (init_result.parse_error) {
    std::fprintf(stderr, "WWE 2K16 launch option error: %s\n", init_result.parse_error->c_str());
    return EXIT_FAILURE;
  }
  if (init_result.exit_requested) return EXIT_SUCCESS;
  auto remaining = std::move(init_result.remaining);
  rex::cvar::ApplyEnvironment();

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL video initialization failed: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }

  int result = EXIT_FAILURE;
  {
    rex::ui::AndroidWindowedAppContext app_context;
    auto creator = rex::ui::WindowedApp::GetCreator("wwe2k16");
    if (!creator) {
      std::fprintf(stderr, "WWE 2K16 app creator not registered\n");
      return EXIT_FAILURE;
    }
    std::unique_ptr<rex::ui::WindowedApp> app = creator(app_context);
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) parsed[option_names[i]] = remaining[i];
    app->SetParsedArguments(std::move(parsed));

    std::filesystem::path log_path = rex::filesystem::GetExecutableFolder() / (app->GetName() + ".log");
    try { rex::InitLogging(log_path.string().c_str()); }
    catch (const spdlog::spdlog_ex& e) {
      std::fprintf(stderr, "Logging init failed: %s\n", e.what());
      rex::InitLogging(nullptr);
    }

    if (app->OnInitialize()) {
      app_context.RunMainSDLLoop();
      result = EXIT_SUCCESS;
    }
    app->InvokeOnDestroy();
  }
  rex::ShutdownLogging();
  SDL_Quit();
  return result;
}
