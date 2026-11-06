// notita sandbox entry point.
//   notita.exe [--scene demo|forge|churn|thermal|empty] [--threads N] [--seed N]
//              [--frames N [--screenshot out.bmp]] [--temp-view]
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
    }
    notita::App app(opts);
    if (!app.init()) return 1;
    return app.run();
}
