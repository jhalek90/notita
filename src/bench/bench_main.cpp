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
//   showcase: the showcase building over the streamed world (16x8 window,
//            everything on incl. electricity): the focus flies east at 2 px/tick
//            from the cave through every ground-floor room to the course, and
//            every switch in the window closes at tick 60 (furnace fuse, gas
//            chamber fuse, heater, electrolysis); then a save and a load
//   electric: large wired areas on a 16x8-chunk window (heat + reactions on,
//            no gas grid): a copper mesh 860x400 px with heater and lamp
//            segments between a column of volt cells and one of ground rods,
//            cut in half by a column of switches that flips every 120 ticks
//            (a fresh solve each time), plus an 800x60 brine pool splitting
//            into hydrogen and oxygen
// Gas rows run the grid on the CPU reference solver (the GPU solver needs
// SDL and a device; notita --gas-selftest times it). "grid ms" is the gas
// grid step (coupling + solve) and is included in the mean.
// churn and flip run with heat off (phase 1 numbers) and on; thermal always
// has heat and the reaction table on.
// Usage: notita_bench [ticks] [threads...]
#include "script/ReactionScript.h"
#include "sim/Bodies.h"
#include "sim/Electric.h"
#include "sim/Particles.h"
#include "sim/Showcase.h"
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
#include <memory>
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
    double simMs = 0, gridMs = 0, p4Ms = 0, active = 0, bodyMs = 0, findMs = 0, bodies = 0, particles = 0;
    int shifts = 0, generated = 0, woken = 0;
    double storeKiB = 0, saveKiB = 0, saveMs = 0, loadMs = 0;
    bool massOk = true;
    double elecMs = 0, lit = 0;
};

WorldResult runWorld(const MaterialRegistry& mats, const ReactionTable& reactions, int threads, int ticks, bool showcase = false) {
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
    // the showcase row: the building laid over the generator, electricity on
    std::unique_ptr<Showcase> show;
    std::unique_ptr<ElectricSystem> elec;
    if (showcase) {
        show = std::make_unique<Showcase>(mats);
        gen.setOverlay(show.get());
        elec = std::make_unique<ElectricSystem>(world, mats, &pool, 2026);
        stream.setElectric(elec.get());
    }
    float fx = showcase ? -900.0f : 0.0f, fy = showcase ? -60.0f : static_cast<float>(gen.surfaceY(0));
    stream.start(static_cast<int>(fx), static_cast<int>(fy));
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
        if (elec) {
            if (i == 60) {
                const MaterialId open = mats.idOf("switch_open");
                for (int y = world.minY(); y < world.minY() + world.heightPx(); ++y)
                    for (int x = world.minX(); x < world.minX() + world.widthPx(); ++x)
                        if (open != kEmpty && world.material(x, y) == open) elec->toggleSwitch(x, y, 0);
            }
            elec->step();
            r.elecMs += elec->stats().ms;
            r.lit += elec->stats().lit;
        }
        bodies.step();
        particles.step();
        grid.step();
        r.simMs += sim.stats().tickMs;
        r.gridMs += grid.stats().solveMs + grid.stats().coupleMs;
        r.p4Ms += bodies.stats().stepMs + particles.stats().stepMs;
        r.bodyMs += bodies.stats().stepMs;
        r.findMs += bodies.stats().findMs;
        r.bodies += bodies.stats().bodies;
        r.particles += static_cast<double>(particles.count());
        r.active += sim.stats().activeChunks;
        if (showcase) fx += 2.0f;
        else {
            fx += 3.0f;
            fy += 1.0f;
        }
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
    if (std::getenv("NOTITA_BENCH_BODY_MATS")) {
        std::vector<long long> hist(mats.size(), 0);
        std::vector<int> aux(4, 0);
        for (const RigidBody& b : bodies.bodies())
            for (const BodyPixel& p : b.pixels) { ++hist[p.cell.material]; }
        for (std::size_t m = 0; m < hist.size(); ++m)
            if (hist[m]) std::printf("  body pixels of %s: %lld\n", mats[static_cast<MaterialId>(m)].name.c_str(), hist[m]);
        for (const RigidBody& b : bodies.bodies()) std::printf("  body %d: %zu px at %.0f,%.0f v %.2f,%.2f\n", b.id, b.pixels.size(), b.x, b.y, b.vx, b.vy);
    }
    std::sort(t.begin(), t.end());
    for (double v : t) r.mean += v;
    r.mean /= static_cast<double>(t.size());
    r.p95 = t[t.size() * 95 / 100];
    r.max = t.back();
    r.shifts = stream.stats().shifts;
    r.shiftMs /= std::max(1, r.shifts);
    r.simMs /= ticks;
    r.gridMs /= ticks;
    r.p4Ms /= ticks;
    r.active /= ticks;
    r.bodyMs /= ticks;
    r.findMs /= ticks;
    r.bodies /= ticks;
    r.particles /= ticks;
    r.elecMs /= ticks;
    r.lit /= ticks;
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

struct ElectricResult {
    double mean = 0, p95 = 0, max = 0, elecMean = 0, elecP95 = 0, elecMax = 0, simMs = 0;
    double nodes = 0, iterations = 0, unsettled = 0, lit = 0, floodMs = 0, solveMs = 0, effectsMs = 0;
    long long splits = 0;
    bool massOk = true;
};

ElectricResult runElectric(const MaterialRegistry& mats, const ReactionTable& reactions, int threads, int ticks) {
    World world(0, 0, 16, 8);
    ThreadPool pool(threads);
    Simulator sim(world, mats, &pool, 2026, &reactions);
    ElectricSystem elec(world, mats, &pool, 2026);
    auto id = [&](const char* n) { return mats.idOf(n); };
    auto put = [&](int x, int y, MaterialId m) { world.place(x, y, m, 128, mats[m].spawnTemp); };
    const MaterialId copper = id("copper"), coil = id("heater_coil"), lamp = id("lamp"), cell = id("volt_cell"),
                     ground = id("ground_rod"), sw = id("switch_closed"), brine = id("brine"), slate = id("slate");
    for (int y = 20; y <= 420; ++y) {
        put(36, y, cell);
        put(905, y, ground);
        for (int x = 40; x <= 900; ++x) {
            const bool row = (y - 20) % 6 == 0, col = (x - 40) % 6 == 0;
            if (!row && !col && x != 470) continue;
            MaterialId m = copper;
            if (row && x % 97 < 3) m = coil;
            else if (col && y % 53 < 4) m = lamp;
            if (x == 470) m = sw;
            put(x, y, m);
        }
        for (int x = 37; x < 40; ++x) put(x, y, copper);
        for (int x = 901; x < 905; ++x) put(x, y, copper);
    }
    for (int x = 96; x <= 904; ++x) put(x, 501, slate);
    for (int y = 436; y <= 500; ++y) { put(96, y, slate); put(904, y, slate); put(97, y, cell); put(903, y, ground); }
    for (int y = 440; y <= 500; ++y)
        for (int x = 98; x <= 902; ++x) put(x, y, brine);
    scenes::pinRect(world, mats, 0, 0, 1023, 511);
    const long long mass0 = world.totalMass(mats);
    std::vector<double> total, el;
    ElectricResult r;
    for (int t = 0; t < ticks; ++t) {
        if (t > 0 && t % 120 == 0) elec.toggleSwitch(470, 20, 0);
        const auto t0 = std::chrono::steady_clock::now();
        sim.tick();
        const auto t1 = std::chrono::steady_clock::now();
        elec.step();
        const auto t2 = std::chrono::steady_clock::now();
        total.push_back(std::chrono::duration<double, std::milli>(t2 - t0).count());
        el.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
        r.simMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
        const ElectricStats& es = elec.stats();
        r.nodes += es.nodes;
        r.floodMs += es.floodMs;
        r.solveMs += es.solveMs;
        r.effectsMs += es.effectsMs;
        r.iterations += es.iterations;
        r.unsettled += es.unsettled;
        r.lit += es.lit;
        r.splits = es.splitsTotal;
        if (std::getenv("NOTITA_BENCH_ELECTRIC_TRACE") && t % 30 == 0)
            std::printf("  t %d: %d comps, %d unsettled, %d it, residual %.4f V, imbalance %.4f\n", t, es.components, es.unsettled,
                        es.iterations, es.residual, es.imbalance);
    }
    r.massOk = world.totalMass(mats) == mass0;
    auto stats = [](std::vector<double> v, double& mean, double& p95, double& mx) {
        double s = 0;
        for (double x : v) s += x;
        mean = s / static_cast<double>(v.size());
        std::sort(v.begin(), v.end());
        p95 = v[static_cast<std::size_t>(static_cast<double>(v.size() - 1) * 0.95)];
        mx = v.back();
    };
    stats(total, r.mean, r.p95, r.max);
    stats(el, r.elecMean, r.elecP95, r.elecMax);
    const double n = static_cast<double>(ticks);
    r.simMs /= n; r.nodes /= n; r.floodMs /= n; r.solveMs /= n; r.effectsMs /= n; r.iterations /= n; r.unsettled /= n; r.lit /= n;
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
    const bool electricOnly = std::getenv("NOTITA_BENCH_ELECTRIC_ONLY") != nullptr;
    const bool showcaseOnly = std::getenv("NOTITA_BENCH_SHOWCASE_ONLY") != nullptr;
    for (const Row& row : rows) {
        if (worldOnly || electricOnly || showcaseOnly) break;
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
    std::printf("\n%-11s %7s %8s %8s %8s %9s %9s %9s %8s %8s %8s %8s %8s %5s\n", "electric", "threads", "mean ms", "p95 ms",
                "max ms", "elec ms", "elec p95", "elec max", "sim ms", "live px", "CG it", "settling", "splits", "mass");
    for (int t : threadCounts) {
        if (worldOnly || showcaseOnly) break;
        const ElectricResult e = runElectric(mats, reactions, t, ticks);
        std::printf("%-11s %7d %8.3f %8.3f %8.3f %9.3f %9.3f %9.3f %8.3f %8.0f %8.1f %8.2f %8lld %5s\n", "electric", t, e.mean, e.p95,
                    e.max, e.elecMean, e.elecP95, e.elecMax, e.simMs, e.nodes, e.iterations, e.unsettled, e.splits,
                    e.massOk ? "exact" : "DRIFT");
        std::printf("%-11s   of which find + flood %.3f ms, solve %.3f ms, effects %.3f ms\n", "", e.floodMs, e.solveMs, e.effectsMs);
    }
    if (electricOnly) return 0;
    std::printf("\n%-11s %7s %8s %8s %8s %7s %9s %9s %8s %9s %8s %9s %5s\n", "world", "threads", "mean ms", "p95 ms", "max ms",
                "shifts", "shift ms", "shift max", "gen/wake", "store KiB", "save KiB", "save/load", "mass");
    for (int t : threadCounts) {
        if (showcaseOnly) break;
        const WorldResult w = runWorld(mats, reactions, t, ticks);
        std::printf("%-11s %7d %8.3f %8.3f %8.3f %7d %9.2f %9.2f %4d/%-3d %9.0f %8.0f %4.0f/%-4.0f %5s\n", "world", t, w.mean, w.p95,
                    w.max, w.shifts, w.shiftMs, w.shiftMax, w.generated, w.woken, w.storeKiB, w.saveKiB, w.saveMs, w.loadMs,
                    w.massOk ? "exact" : "DRIFT");
        std::printf("%-11s   of which sim %.3f ms (%.1f of 128 chunks moving), grid %.3f ms (CPU solver, 256x128), particles+bodies %.3f ms\n",
                    "", w.simMs, w.active, w.gridMs, w.p4Ms);
        std::printf("%-11s   bodies %.3f ms (clump search %.3f, %.1f bodies), %.0f particles alive\n", "", w.bodyMs, w.findMs,
                    w.bodies, w.particles);
    }
    std::printf("\n%-11s %7s %8s %8s %8s %7s %9s %9s %8s %8s %9s %8s %9s %5s\n", "showcase", "threads", "mean ms", "p95 ms", "max ms",
                "shifts", "shift ms", "shift max", "elec ms", "lit px", "store KiB", "save KiB", "save/load", "mass");
    for (int t : threadCounts) {
        const WorldResult w = runWorld(mats, reactions, t, ticks, true);
        std::printf("%-11s %7d %8.3f %8.3f %8.3f %7d %9.2f %9.2f %8.3f %8.0f %9.0f %8.0f %4.0f/%-4.0f %5s\n", "showcase", t, w.mean, w.p95,
                    w.max, w.shifts, w.shiftMs, w.shiftMax, w.elecMs, w.lit, w.storeKiB, w.saveKiB, w.saveMs, w.loadMs,
                    w.massOk ? "exact" : "DRIFT");
        std::printf("%-11s   of which sim %.3f ms (%.1f of 128 chunks moving), grid %.3f ms (CPU solver), particles+bodies %.3f ms (%.1f bodies)\n",
                    "", w.simMs, w.active, w.gridMs, w.p4Ms, w.bodies);
    }
    std::printf("\nmotion / heat = average chunks running the motion / heat pass (of 60); events = state changes,\n"
                "ignitions, burn-outs and reactions per tick; grid ms = gas grid step (CPU solver, 160x96 cells,\n"
                "40 pressure iterations), included in the mean; solve ms = the solver part of it\n");
    return 0;
}
