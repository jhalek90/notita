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
//   quarry:  the phase 4 playground (waterfall splashes, blending liquids,
//            blocks falling off a burning post) with particles and bodies on
//   thermal+p4: thermal+gas with splash particles and rigid bodies on (the
//            frost/timber/iron clumps in the mix become bodies)
//   flip+p4: flip (heat off) with splash particles on: every liquid landing hard splashes
//   play:    the phase 5 playground with everything on and a scripted player
//            walking, digging, jetting and setting off charges
//   world:   phase 6: the streamed procedural world (16x8 awake chunks, like
//            the app) with everything on, the focus flying right at 3 px/tick
//            and diving 1 px/tick, so a column of chunks sleeps and one wakes
//            (or is generated) every ~21 ticks; then a save and a load
// Gas rows run the grid on the CPU reference solver (the GPU solver needs
// SDL and a device; notita --gas-selftest times it). "grid ms" is the gas
// grid step (coupling + solve) and is included in the mean.
// churn and flip run with heat off (phase 1 numbers) and on; thermal always
// has heat and the reaction table on.
// Usage: notita_bench [ticks] [threads...]
#include "script/ReactionScript.h"
#include "sim/Bodies.h"
#include "sim/Particles.h"
#include "sim/Player.h"
#include "sim/GasGrid.h"
#include "sim/Material.h"
#include "sim/Reactions.h"
#include "sim/Scenes.h"
#include "sim/Simulator.h"
#include "sim/Streamer.h"
#include "sim/WorldGen.h"

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
    double particleMs = 0, bodyMs = 0, particles = 0, bodies = 0, bodyPixels = 0;
    double playerMs = 0;
};

enum class Scene { Churn, Flip, Thermal, Conduct, GasLab, Quarry, Play };

Result runScene(const MaterialRegistry& mats, const ReactionTable& reactions, Scene scene, bool heat, bool gas,
                bool phase4, int threads, int ticks) {
    World world(0, 0, 10, 6);
    ThreadPool pool(threads);
    Simulator sim(world, mats, &pool, 2026, &reactions);
    GasGrid grid(world, mats, &pool, 2026);
    ParticleSystem particles(world, mats, &pool, 2026);
    BodySystem bodies(world, mats, &particles, 2026);
    Player player(world, mats, 2026);
    if (gas) player.setGas(&grid);
    if (phase4) player.setBodies(&bodies);
    if (gas) {
        sim.setGasExchange(&grid);
        sim.params().ambientRate = 0.0f;
    }
    if (phase4) {
        sim.setParticles(&particles);
        if (gas) {
            particles.setGas(&grid);
            grid.setParticles(&particles);
            bodies.setGas(&grid);
        }
    }
    sim.params().heat = heat;
    sim.params().reactions = heat;
    if (scene == Scene::Thermal) scenes::thermalChurn(world, mats, 77);
    else if (scene == Scene::GasLab) scenes::gasLab(world, mats, 77);
    else if (scene == Scene::Quarry) scenes::quarry(world, mats, 77);
    else if (scene == Scene::Play) {
        float sx = 0, sy = 0;
        scenes::playground(world, mats, 77, &sx, &sy);
        player.spawn(sx, sy);
        player.inventory().capacity = 1000000;
        player.inventory().add(mats.idOf("water"), 100000);
        player.inventory().charges = 1000000;
    }
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
    int tick = 0;
    double playerMs = 0;
    auto step = [&] {
        sim.tick();
        if (phase4) {
            bodies.step();
            particles.step();
        }
        double g = 0.0;
        if (gas) {
            const auto t0 = std::chrono::steady_clock::now();
            grid.step();
            g = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        playerMs = 0.0;
        if (scene == Scene::Play) {
            // walk back and forth; dig ahead, jet up, set off a charge at the feet, in turn
            const int t = tick++;
            PlayerInput in;
            in.move = (t / 240) % 2 ? -1.0f : 1.0f;
            in.material = mats.idOf("water");
            const int phase = t % 120;
            in.tool = phase < 40 ? Tool::Dig : phase < 80 ? Tool::Jet : Tool::Blast;
            in.use = phase < 80;
            in.usePressed = phase == 80;
            in.aimX = player.x() + in.move * 20.0f;
            in.aimY = in.tool == Tool::Jet ? player.y() + 40.0f : in.tool == Tool::Blast ? player.y() + 2.0f : player.centreY() + 8.0f;
            const auto t1 = std::chrono::steady_clock::now();
            player.step(in);
            playerMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        }
        return g;
    };
    if (flip) for (int i = 0; i < 300; ++i) step(); // let the first fill settle a bit
    for (int i = 0; i < ticks; ++i) {
        if (flip && i % 90 == 0) scenes::flipVertical(world);
        const double g = step();
        const SimStats& st = sim.stats();
        const double p4 = phase4 ? particles.stats().stepMs + bodies.stats().stepMs : 0.0;
        ms.push_back(st.tickMs + g + p4 + playerMs);
        r.playerMs += playerMs;
        r.gridMs += g;
        r.particleMs += phase4 ? particles.stats().stepMs : 0.0;
        r.bodyMs += phase4 ? bodies.stats().stepMs : 0.0;
        r.particles += static_cast<double>(particles.count());
        r.bodies += bodies.stats().bodies;
        r.bodyPixels += bodies.stats().pixels;
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
    r.particleMs /= n;
    r.bodyMs /= n;
    r.particles /= n;
    r.bodies /= n;
    r.bodyPixels /= n;
    r.playerMs /= n;
    return r;
}

struct WorldResult {
    double mean = 0, p95 = 0, max = 0, shiftMs = 0, shiftMax = 0;
    int shifts = 0, generated = 0, woken = 0;
    double storeKiB = 0, saveKiB = 0, saveMs = 0, loadMs = 0;
    bool massOk = true;
};

WorldResult runWorld(const MaterialRegistry& mats, const ReactionTable& reactions, int threads, int ticks) {
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    StreamParams sp; // 16 x 8, the app's window
    WorldGen gen(mats, 2026);
    World world(0, 0, sp.chunksW, sp.chunksH);
    ThreadPool pool(threads);
    Simulator sim(world, mats, &pool, 2026, &reactions);
    GasGrid grid(world, mats, &pool, 2026);
    sim.setGasExchange(&grid);
    sim.params().ambientRate = 0.0f;
    ParticleSystem particles(world, mats, &pool, 2026);
    BodySystem bodies(world, mats, &particles, 2026);
    sim.setParticles(&particles);
    particles.setGas(&grid);
    grid.setParticles(&particles);
    bodies.setGas(&grid);
    Streamer stream(world, mats, gen, sp);
    stream.attach(&sim, &grid, &particles, &bodies);
    stream.setPool(&pool);
    float fx = 0.0f, fy = static_cast<float>(gen.surfaceY(0));
    stream.start(0, static_cast<int>(fy));
    auto conserved = [&] {
        const long long px = world.totalMass(mats) + particles.totalMass() + bodies.totalMass() + stream.storedMass() -
                             stream.generatedMass();
        return px * kGasMassOne + grid.massFixed() + stream.storedGasFixed();
    };
    const long long m0 = conserved();
    WorldResult r;
    std::vector<double> t;
    for (int i = 0; i < ticks; ++i) {
        const auto t0 = clock::now();
        sim.tick();
        bodies.step();
        particles.step();
        grid.step();
        fx += 3.0f;
        fy += 1.0f;
        const int before = stream.stats().shifts;
        stream.follow(fx, fy);
        if (stream.stats().shifts != before) {
            r.shiftMs += stream.stats().lastShiftMs;
            r.shiftMax = std::max(r.shiftMax, stream.stats().lastShiftMs);
        }
        t.push_back(ms(t0, clock::now()));
        if (conserved() != m0 && r.massOk) {
            r.massOk = false;
            std::printf("  mass drift at tick %d (shift this tick: %d): %lld -> %lld; world %lld particles %lld bodies %lld stored %lld gen %lld grid %lld storedGas %lld\n",
                        i, stream.stats().shifts != before, m0, conserved(), world.totalMass(mats), particles.totalMass(), bodies.totalMass(),
                        stream.storedMass(), stream.generatedMass(), static_cast<long long>(grid.massFixed()), static_cast<long long>(stream.storedGasFixed()));
        }
    }
    std::sort(t.begin(), t.end());
    for (double v : t) r.mean += v;
    r.mean /= static_cast<double>(t.size());
    r.p95 = t[t.size() * 95 / 100];
    r.max = t.back();
    r.shifts = stream.stats().shifts;
    r.shiftMs /= std::max(1, r.shifts);
    r.generated = stream.stats().generated;
    r.woken = stream.stats().woken;
    r.storeKiB = static_cast<double>(stream.stats().storeBytes) / 1024.0;
    std::vector<std::uint8_t> bytes;
    SaveExtras ex;
    auto s0 = clock::now();
    stream.saveToBytes(bytes, ex);
    r.saveMs = ms(s0, clock::now());
    r.saveKiB = static_cast<double>(bytes.size()) / 1024.0;
    s0 = clock::now();
    stream.loadFromBytes(bytes, ex);
    r.loadMs = ms(s0, clock::now());
    if (conserved() != m0) r.massOk = false;
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
    struct Row { const char* name; Scene scene; bool heat; bool gas; bool phase4; };
    const Row rows[] = {
        {"churn", Scene::Churn, false, false, false},    {"churn", Scene::Churn, true, false, false},
        {"flip", Scene::Flip, false, false, false},      {"flip", Scene::Flip, true, false, false},
        {"thermal", Scene::Thermal, true, false, false}, {"conduct", Scene::Conduct, true, false, false},
        {"thermal+gas", Scene::Thermal, true, true, false}, {"gaslab", Scene::GasLab, true, true, false},
        {"quarry", Scene::Quarry, true, true, true},       {"thermal+p4", Scene::Thermal, true, true, true},
        {"flip+p4", Scene::Flip, false, false, true},     {"play", Scene::Play, true, true, true},
    };
    const bool gasOnly = std::getenv("NOTITA_BENCH_GAS_ONLY") != nullptr;
    const bool p4Only = std::getenv("NOTITA_BENCH_P4_ONLY") != nullptr;
    const bool playOnly = std::getenv("NOTITA_BENCH_PLAY_ONLY") != nullptr;
    const bool worldOnly = std::getenv("NOTITA_BENCH_WORLD_ONLY") != nullptr;
    for (const Row& row : rows) {
        if (worldOnly) break;
        if (gasOnly && !row.gas) continue;
        if (p4Only && !row.phase4) continue;
        if (playOnly && row.scene != Scene::Play) continue;
        for (int t : threadCounts) {
            const Result r = runScene(mats, reactions, row.scene, row.heat, row.gas, row.phase4, t, ticks);
            std::printf("%-11s %4s %4s %7d %8.3f %8.3f %8.3f %8.3f %7.1f %7.1f %10.0f %8.0f %8.3f %8.3f\n", row.name,
                        row.heat ? "on" : "off", row.gas ? "on" : "off", t, r.mean, r.p50, r.p95, r.max, r.activeChunks,
                        r.heatChunks, r.cells, r.events, r.gridMs, r.solveMs);
            if (row.gas)
                std::printf("%-11s   grid coupling: obstacles + gas pixels %.3f ms, push on pixels %.3f ms, rest %.3f ms\n", "",
                            r.obstaclesMs, r.pushMs, r.gridMs - r.solveMs - r.obstaclesMs - r.pushMs);
            if (row.phase4)
                std::printf("%-11s   particles %.3f ms (%.0f alive), bodies %.3f ms (%.1f bodies, %.0f px)\n", "",
                            r.particleMs, r.particles, r.bodyMs, r.bodies, r.bodyPixels);
            if (row.scene == Scene::Play) std::printf("%-11s   player %.3f ms\n", "", r.playerMs);
        }
    }
    std::printf("\n%-11s %7s %8s %8s %8s %7s %9s %9s %8s %9s %8s %9s %5s\n", "world", "threads", "mean ms", "p95 ms", "max ms",
                "shifts", "shift ms", "shift max", "gen/wake", "store KiB", "save KiB", "save/load", "mass");
    for (int t : threadCounts) {
        const WorldResult w = runWorld(mats, reactions, t, ticks);
        std::printf("%-11s %7d %8.3f %8.3f %8.3f %7d %9.2f %9.2f %4d/%-3d %9.0f %8.0f %4.0f/%-4.0f %5s\n", "world", t, w.mean, w.p95,
                    w.max, w.shifts, w.shiftMs, w.shiftMax, w.generated, w.woken, w.storeKiB, w.saveKiB, w.saveMs, w.loadMs,
                    w.massOk ? "exact" : "DRIFT");
    }
    std::printf("\nmotion / heat = average chunks running the motion / heat pass (of 60); events = state changes,\n"
                "ignitions, burn-outs and reactions per tick; grid ms = gas grid step (CPU solver, 160x96 cells,\n"
                "40 pressure iterations), included in the mean; solve ms = the solver part of it\n");
    return 0;
}
