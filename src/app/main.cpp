// notita sandbox entry point.
//   notita.exe [--scene demo|forge|churn|thermal|gas|quarry|play|empty] [--threads N] [--seed N]
//              [--frames N [--screenshot out.bmp]] [--temp-view]
//              [--gas-cpu] [--gas-view gas|velocity|pressure] [--gas-selftest]
//              [--scene world] [--load notita_world.sav]   (phase 6: the streamed world, F8 save / F9 load)
//              [--autoplay]   (the play scene with the player driven through a short demo)
//              [--no-light] [--light-cpu] [--light-selftest]
#include <SDL3/SDL_main.h>

#include "App.h"

#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    notita::AppOptions opts;
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--scene")) opts.scene = next();
        else if (!std::strcmp(argv[i], "--threads")) opts.threads = std::atoi(next());
        else if (!std::strcmp(argv[i], "--seed")) opts.seed = std::strtoull(next(), nullptr, 10);
        else if (!std::strcmp(argv[i], "--frames")) opts.frames = std::atoi(next());
        else if (!std::strcmp(argv[i], "--screenshot")) opts.screenshot = next();
        else if (!std::strcmp(argv[i], "--temp-view")) opts.tempView = true;
        else if (!std::strcmp(argv[i], "--gas-cpu")) opts.gasCpu = true;
        else if (!std::strcmp(argv[i], "--gas-selftest")) opts.gasSelfTest = true;
        else if (!std::strcmp(argv[i], "--no-light")) opts.noLight = true;
        else if (!std::strcmp(argv[i], "--light-cpu")) opts.lightCpu = true;
        else if (!std::strcmp(argv[i], "--light-selftest")) opts.lightSelfTest = true;
        else if (!std::strcmp(argv[i], "--load")) { opts.load = next(); opts.scene = "world"; }
        else if (!std::strcmp(argv[i], "--autoplay")) { opts.autoplay = true; if (opts.scene == "demo") opts.scene = "play"; }
        else if (!std::strcmp(argv[i], "--gas-view")) {
            const char* v = next();
            opts.gasView = !std::strcmp(v, "velocity") ? 1 : !std::strcmp(v, "pressure") ? 2 : 0;
        }
    }
    notita::App app(opts);
    if (!app.init()) return 1;
    if (opts.gasSelfTest) return app.gasSelfTest();
    if (opts.lightSelfTest) return app.lightSelfTest();
    return app.run();
}
