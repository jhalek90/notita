// Headless benchmark: a 640x384 world (10x6 chunks, a bit more than the
// 640x360 view) completely full of moving material.
//   churn: 70% of all pixels are a random mix of powders/liquids/gas that
//          fall, sort and slosh (worst case right after the fill)
//   flip:  sustained load: the same world is turned upside down every 90
//          ticks, so the whole screen keeps falling and re-sorting
// Usage: notita_bench [ticks] [threads...]
#include "sim/Material.h"
#include "sim/Scenes.h"
#include "sim/Simulator.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace notita;

namespace {

struct Result {
    double mean = 0, p50 = 0, p95 = 0, max = 0;
    double activeChunks = 0, cells = 0;
};

Result summarize(std::vector<double>& ms, double activeSum, double cellSum) {
    Result r;
    std::sort(ms.begin(), ms.end());
    for (double v : ms) r.mean += v;
    r.mean /= static_cast<double>(ms.size());
    r.p50 = ms[ms.size() / 2];
    r.p95 = ms[std::min(ms.size() - 1, ms.size() * 95 / 100)];
    r.max = ms.back();
    r.activeChunks = activeSum / static_cast<double>(ms.size());
    r.cells = cellSum / static_cast<double>(ms.size());
    return r;
}

Result runScene(const MaterialRegistry& mats, bool flip, int threads, int ticks) {
    World world(0, 0, 10, 6);
    ThreadPool pool(threads);
    Simulator sim(world, mats, &pool, 2026);
    scenes::churn(world, mats, 77);
    std::vector<double> ms;
    double active = 0, cells = 0;
    if (flip) for (int i = 0; i < 300; ++i) sim.tick(); // let the first fill settle a bit
    for (int i = 0; i < ticks; ++i) {
        if (flip && i % 90 == 0) scenes::flipVertical(world);
        sim.tick();
        ms.push_back(sim.stats().tickMs);
        active += sim.stats().activeChunks;
        cells += static_cast<double>(sim.stats().cellsProcessed);
    }
    return summarize(ms, active, cells);
}

} // namespace

int main(int argc, char** argv) {
    const int ticks = argc > 1 ? std::max(10, std::atoi(argv[1])) : 600;
    std::vector<int> threadCounts;
    for (int i = 2; i < argc; ++i) threadCounts.push_back(std::max(1, std::atoi(argv[i])));
    const int hw = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    if (threadCounts.empty()) threadCounts = {1, 4, hw};

    std::vector<std::string> errors;
    MaterialRegistry mats = MaterialRegistry::loadDefault(&errors);
    for (auto& e : errors) std::printf("materials: %s\n", e.c_str());

    std::printf("notita sim benchmark: world 640x384 (10x6 chunks of 64x64), %d ticks per run, %d hw threads\n", ticks, hw);
    std::printf("tick budget at 60 Hz: 16.67 ms\n\n");
    std::printf("%-6s %8s %9s %9s %9s %9s %9s %12s\n", "scene", "threads", "mean ms", "p50 ms", "p95 ms", "max ms", "chunks", "cells/tick");
    for (int scene = 0; scene < 2; ++scene)
        for (int t : threadCounts) {
            Result r = runScene(mats, scene == 1, t, ticks);
            std::printf("%-6s %8d %9.3f %9.3f %9.3f %9.3f %9.1f %12.0f\n", scene ? "flip" : "churn", t,
                        r.mean, r.p50, r.p95, r.max, r.activeChunks, r.cells);
        }
    return 0;
}
