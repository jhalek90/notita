// Headless benchmark: a 640x384 world (10x6 chunks, a bit more than the
// 640x360 view) completely full of moving material.
//   churn:   70% of all pixels are a random mix of powders/liquids/gas that
//            fall, sort and slosh (worst case right after the fill)
//   flip:    sustained load: the same world is turned upside down every 90
//            ticks, so the whole screen keeps falling and re-sorting
//   thermal: churn with magma, frost, timber, etchant and iron in the mix at
//            their spawn temperatures: boiling, freezing, burning and
//            reactions everywhere, plus the flip every 90 ticks
//   conduct: solid slate at random temperatures: heat only, nothing moves
//   gaslab:  the phase 3 gas playground (boiling, a fire, a hydrogen cave,
//            heavy gas) with the gas grid on
//   thermal+gas: thermal with the gas grid taking every gas it makes
// Gas rows run the grid on the CPU reference solver (the GPU solver needs
// SDL and a device; notita --gas-selftest times it). "grid ms" is the gas
// grid step (coupling + solve) and is included in the mean.
// churn and flip run with heat off (phase 1 numbers) and on; thermal always
// has heat and the reaction table on.
// Usage: notita_bench [ticks] [threads...]
#include "script/ReactionScript.h"
#include "sim/GasGrid.h"
#include "sim/Material.h"
#include "sim/Reactions.h"
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
    double activeChunks = 0, heatChunks = 0, cells = 0, events = 0;
    double gridMs = 0, solveMs = 0, obstaclesMs = 0, pushMs = 0;
};

enum class Scene { Churn, Flip, Thermal, Conduct, GasLab };

Result runScene(const MaterialRegistry& mats, const ReactionTable& reactions, Scene scene, bool heat, bool gas,
                int threads, int ticks) {
    World world(0, 0, 10, 6);
    ThreadPool pool(threads);
    Simulator sim(world, mats, &pool, 2026, &reactions);
    GasGrid grid(world, mats, &pool, 2026);
    if (gas) {
        sim.setGasExchange(&grid);
        sim.params().ambientRate = 0.0f;
    }
    sim.params().heat = heat;
    sim.params().reactions = heat;
    if (scene == Scene::Thermal) scenes::thermalChurn(world, mats, 77);
    else if (scene == Scene::GasLab) scenes::gasLab(world, mats, 77);
    else if (scene == Scene::Conduct) {
        // solid slate at random temperatures: nothing moves, only heat flows
        Rng rng(77);
        for (Chunk* ch : world.chunks())
            for (Cell& c : ch->cells) {
                c = mats.find("slate")->spawn(static_cast<std::uint8_t>(rng.below(256)));
                c.temperature = static_cast<float>(rng.below(1000));
            }
        world.wakeAll();
    } else scenes::churn(world, mats, 77);
    const bool flip = scene == Scene::Flip || scene == Scene::Thermal;
    std::vector<double> ms;
    Result r;
    auto step = [&] {
        sim.tick();
        if (!gas) return 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        grid.step();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    if (flip) for (int i = 0; i < 300; ++i) step(); // let the first fill settle a bit
    for (int i = 0; i < ticks; ++i) {
        if (flip && i % 90 == 0) scenes::flipVertical(world);
        const double g = step();
        const SimStats& st = sim.stats();
        ms.push_back(st.tickMs + g);
        r.gridMs += g;
        r.solveMs += grid.stats().solveMs;
        r.obstaclesMs += grid.stats().obstaclesMs;
        r.pushMs += grid.stats().pushMs;
        r.activeChunks += st.activeChunks;
        r.heatChunks += st.heatChunks;
        r.cells += static_cast<double>(st.cellsProcessed);
        r.events += static_cast<double>(st.events);
    }
    std::sort(ms.begin(), ms.end());
    for (double v : ms) r.mean += v;
    const double n = static_cast<double>(ms.size());
    r.mean /= n;
    r.p50 = ms[ms.size() / 2];
    r.p95 = ms[std::min(ms.size() - 1, ms.size() * 95 / 100)];
    r.max = ms.back();
    r.activeChunks /= n;
    r.heatChunks /= n;
    r.cells /= n;
    r.events /= n;
    r.gridMs /= n;
    r.solveMs /= n;
    r.obstaclesMs /= n;
    r.pushMs /= n;
    return r;
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
    ReactionTable reactions(mats);
    loadDefaultReactions(reactions, &errors);
    for (auto& e : errors) std::printf("data: %s\n", e.c_str());

    std::printf("notita sim benchmark: world 640x384 (10x6 chunks of 64x64), %d ticks per run, %d hw threads\n", ticks, hw);
    std::printf("%zu materials, %zu reactions; tick budget at 60 Hz: 16.67 ms\n\n", mats.size(), reactions.size());
    std::printf("%-11s %4s %4s %7s %8s %8s %8s %8s %7s %7s %10s %8s %8s %8s\n", "scene", "heat", "gas", "threads",
                "mean ms", "p50 ms", "p95 ms", "max ms", "motion", "heat", "cells/tick", "events", "grid ms", "solve ms");
    struct Row { const char* name; Scene scene; bool heat; bool gas; };
    const Row rows[] = {
        {"churn", Scene::Churn, false, false},    {"churn", Scene::Churn, true, false},
        {"flip", Scene::Flip, false, false},      {"flip", Scene::Flip, true, false},
        {"thermal", Scene::Thermal, true, false}, {"conduct", Scene::Conduct, true, false},
        {"thermal+gas", Scene::Thermal, true, true}, {"gaslab", Scene::GasLab, true, true},
    };
    const bool gasOnly = std::getenv("NOTITA_BENCH_GAS_ONLY") != nullptr;
    for (const Row& row : rows) {
        if (gasOnly && !row.gas) continue;
        for (int t : threadCounts) {
            const Result r = runScene(mats, reactions, row.scene, row.heat, row.gas, t, ticks);
            std::printf("%-11s %4s %4s %7d %8.3f %8.3f %8.3f %8.3f %7.1f %7.1f %10.0f %8.0f %8.3f %8.3f\n", row.name,
                        row.heat ? "on" : "off", row.gas ? "on" : "off", t, r.mean, r.p50, r.p95, r.max, r.activeChunks,
                        r.heatChunks, r.cells, r.events, r.gridMs, r.solveMs);
            if (row.gas)
                std::printf("%-11s   grid coupling: obstacles + gas pixels %.3f ms, push on pixels %.3f ms, rest %.3f ms\n", "",
                            r.obstaclesMs, r.pushMs, r.gridMs - r.solveMs - r.obstaclesMs - r.pushMs);
        }
    }
    std::printf("\nmotion / heat = average chunks running the motion / heat pass (of 60); events = state changes,\n"
                "ignitions, burn-outs and reactions per tick; grid ms = gas grid step (CPU solver, 160x96 cells,\n"
                "40 pressure iterations), included in the mean; solve ms = the solver part of it\n");
    return 0;
}
