// Fine Grain (repo and exe: notita) entry point.
//   notita.exe [--scene showcase|topdown|world|demo|forge|churn|thermal|gas|quarry|play|circuits|empty] [--threads N] [--seed N]
//              (default: showcase, the concrete building over the streamed world)
//              [--frames N [--screenshot out.bmp]] [--temp-view] [--electric-view current|potential]
//              [--gas-cpu] [--gas-view gas|velocity|pressure] [--gas-selftest]
//              [--scene world] [--load notita_world.sav]   (phase 6: the streamed world, F8 save / F9 load)
//              [--autoplay]   (the play scene with the player driven through a short demo)
//              [--no-light] [--light-cpu] [--light-selftest]
//              [--old-look] [--old-motion] [--lockstep]   (#477: liquids as pixels / phase 1 powder and liquid rules; F10 flips)
//              [--old-flames]  (#480: flames as pixels instead of glowing gas on the grid; F11 flips)
//              [--at x,y]     (start with the player's feet at x,y: screenshots of one room)
//              [--tab Name] [--no-hud] [--no-menu]  (#484: open the menu on a tab, e.g. --tab Sim; start without the HUD / menu)
#include <SDL3/SDL_main.h>

#include "App.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    notita::AppOptions opts;
    bool sceneGiven = false;
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--scene")) { opts.scene = next(); sceneGiven = true; }
        else if (!std::strcmp(argv[i], "--threads")) opts.threads = std::atoi(next());
        else if (!std::strcmp(argv[i], "--seed")) opts.seed = std::strtoull(next(), nullptr, 10);
        else if (!std::strcmp(argv[i], "--frames")) opts.frames = std::atoi(next());
        else if (!std::strcmp(argv[i], "--screenshot")) opts.screenshot = next();
        else if (!std::strcmp(argv[i], "--temp-view")) opts.tempView = true;
        else if (!std::strcmp(argv[i], "--electric-view")) opts.electricView = !std::strcmp(next(), "potential") ? 1 : 0;
        else if (!std::strcmp(argv[i], "--gas-cpu")) opts.gasCpu = true;
        else if (!std::strcmp(argv[i], "--gas-selftest")) opts.gasSelfTest = true;
        else if (!std::strcmp(argv[i], "--no-light")) opts.noLight = true;
        else if (!std::strcmp(argv[i], "--old-look")) opts.oldLook = true;     // #477: liquids as pixels
        else if (!std::strcmp(argv[i], "--old-motion")) opts.oldMotion = true; // #477: phase 1 powder/liquid rules
        else if (!std::strcmp(argv[i], "--lockstep")) opts.lockstep = true;    // one tick per frame
        else if (!std::strcmp(argv[i], "--old-flames")) opts.oldFlames = true; // #480: flames as pixels
        else if (!std::strcmp(argv[i], "--no-relief")) opts.noRelief = true; // #481: no surface relief / wet look
        else if (!std::strcmp(argv[i], "--light-cpu")) opts.lightCpu = true;
        else if (!std::strcmp(argv[i], "--light-selftest")) opts.lightSelfTest = true;
        else if (!std::strcmp(argv[i], "--load")) { opts.load = next(); opts.scene = "world"; }
        else if (!std::strcmp(argv[i], "--autoplay")) opts.autoplay = true;
        else if (!std::strcmp(argv[i], "--tab")) opts.tab = next();       // #484: open the menu on this tab
        else if (!std::strcmp(argv[i], "--no-hud")) opts.noHud = true;
        else if (!std::strcmp(argv[i], "--no-menu")) opts.noMenu = true;
        else if (!std::strcmp(argv[i], "--at")) {
            opts.at = std::sscanf(next(), "%f,%f", &opts.atX, &opts.atY) == 2;
        }
        else if (!std::strcmp(argv[i], "--gas-view")) {
            const char* v = next();
            opts.gasView = !std::strcmp(v, "velocity") ? 1 : !std::strcmp(v, "pressure") ? 2 : 0;
        }
    }
    if (opts.autoplay && !sceneGiven && opts.load.empty()) opts.scene = "play"; // plain --autoplay is the play-scene demo
    notita::App app(opts);
    if (!app.init()) return 1;
    if (opts.gasSelfTest) return app.gasSelfTest();
    if (opts.lightSelfTest) return app.lightSelfTest();
    return app.run();
}
