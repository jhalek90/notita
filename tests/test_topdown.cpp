// #483: top-down regions (src/sim/Gravity.h). Inside a FlatRegion gravity
// points into the screen: liquids don't fall (they hold as puddles and run
// down the region's slope), powders rest until pushed, gases don't rise (CA
// pixels and the gas grid), and mass and determinism rules are unchanged.
#include "test.h"

#include "sim/GasGrid.h"
#include "sim/Material.h"
#include "sim/Player.h"
#include "sim/Scenes.h"
#include "sim/Simulator.h"
#include "sim/ThreadPool.h"
#include "sim/World.h"

#include <cmath>
#include <memory>

using namespace notita;

namespace {

const char* kFlatMaterials = R"(
[stone]
kind = solid
density = 2600
color = #808080

[sand]
kind = powder
density = 1600
slide = 0.9
color = #d0b060

[water]
kind = liquid
density = 1000
spread = 6
color = #3070c0

[oil]
kind = liquid
density = 800
viscosity = 0.1
spread = 5
color = #806020

[lightgas]
kind = gas
density = 0.5
spread = 2
color = #e0e0e0
)";

struct Flat {
    MaterialRegistry mats;
    std::unique_ptr<World> world;
    std::unique_ptr<ThreadPool> pool;
    std::unique_ptr<Simulator> sim;
    Rng rng{11};

    Flat(int chunksW, int chunksH, int threads = 4, std::uint64_t seed = 3) {
        std::vector<std::string> errors;
        mats.loadFromString(kFlatMaterials, &errors);
        CHECK(errors.empty());
        world = std::make_unique<World>(0, 0, chunksW, chunksH);
        pool = std::make_unique<ThreadPool>(threads);
        sim = std::make_unique<Simulator>(*world, mats, pool.get(), seed);
    }
    MaterialId id(const char* n) const { return mats.idOf(n); }
    void fill(const char* m, int x0, int y0, int x1, int y1) { scenes::fillRect(*world, id(m), x0, y0, x1, y1, rng); }
    void run(int ticks) { for (int i = 0; i < ticks; ++i) sim->tick(); }
    void flatAll(int slopeX = 0, int slopeY = 0) {
        world->gravity().set({{0, 0, world->widthPx() - 1, world->heightPx() - 1, slopeX, slopeY}});
    }
    struct Spread { std::size_t n = 0; double mx = 0, my = 0; int x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30); };
    Spread spread(MaterialId m) const {
        Spread s;
        for (int y = 0; y < world->heightPx(); ++y)
            for (int x = 0; x < world->widthPx(); ++x)
                if (world->material(x, y) == m) {
                    ++s.n; s.mx += x; s.my += y;
                    s.x0 = std::min(s.x0, x); s.x1 = std::max(s.x1, x);
                    s.y0 = std::min(s.y0, y); s.y1 = std::max(s.y1, y);
                }
        if (s.n) { s.mx /= static_cast<double>(s.n); s.my /= static_cast<double>(s.n); }
        return s;
    }
};

} // namespace

TEST_CASE(topdown_liquid_holds_as_a_puddle_and_does_not_fall) {
    // the same block of water with nothing under it: side view it falls, from above it stays
    Flat side(3, 3), top(3, 3);
    top.flatAll();
    for (Flat* f : {&side, &top}) f->fill("water", 80, 40, 110, 60);
    const auto s0 = top.spread(top.id("water"));
    side.run(240);
    top.run(240);
    const auto s1 = side.spread(side.id("water")), t1 = top.spread(top.id("water"));
    CHECK_EQ(t1.n, s0.n);                        // every pixel kept
    CHECK(s1.my > s0.my + 60.0);                 // side view: on the floor
    CHECK(std::abs(t1.my - s0.my) < 2.0);        // from above: it didn't go anywhere
    CHECK(std::abs(t1.mx - s0.mx) < 2.0);
    CHECK(t1.y1 - t1.y0 <= s0.y1 - s0.y0 + 4);   // it stays one puddle (no film over the floor)
    CHECK(t1.x1 - t1.x0 <= s0.x1 - s0.x0 + 4);
    // and it settles: a calm puddle lets its chunks sleep
    top.run(400);
    top.sim->tick();
    CHECK_EQ(top.sim->stats().activeChunks, 0);
}

TEST_CASE(topdown_liquid_runs_down_a_slope_and_spreads_on_the_level) {
    Flat f(4, 3);
    const int W = f.world->widthPx();
    // a sloped gutter (downhill to +x) first, then the level floor around it
    f.world->gravity().set({{0, 60, 120, 100, 24, 0}, {0, 0, W - 1, f.world->heightPx() - 1, 0, 0}});
    f.fill("stone", 0, 58, 120, 59);
    f.fill("stone", 0, 101, 120, 102);
    f.fill("water", 4, 64, 34, 96);
    const auto s0 = f.spread(f.id("water"));
    f.run(400);
    const auto s1 = f.spread(f.id("water"));
    CHECK_EQ(s1.n, s0.n);
    std::printf("  slope: mean x %.1f -> %.1f, x %d..%d -> %d..%d, y %d..%d -> %d..%d\n", s0.mx, s1.mx, s0.x0, s0.x1, s1.x0, s1.x1,
                s0.y0, s0.y1, s1.y0, s1.y1);
    CHECK(s1.mx > s0.mx + 60.0);                 // it ran down the gutter...
    CHECK(s1.x1 > 121);                          // ...out of its end onto the level floor...
    CHECK(s1.y1 - s1.y0 > s0.y1 - s0.y0 + 4);    // ...and fanned out there (wider than the gutter)
    CHECK(std::abs(s1.my - s0.my) < 25.0);       // no gravity across the slope
}

TEST_CASE(topdown_powder_rests_until_pushed) {
    Flat f(3, 3);
    f.flatAll();
    f.fill("sand", 60, 60, 90, 90);
    const std::uint64_t h0 = f.world->hash();
    f.run(60);
    CHECK(f.world->hash() == h0);                // a pile seen from above lies still
    f.sim->tick();
    CHECK_EQ(f.sim->stats().activeChunks, 0);
    // a shove east along one row: the grains slide out of the pile, deflect round
    // the ones in front, and stop
    for (int x = 80; x <= 90; ++x) {
        f.world->cell(x, 75)->vx = static_cast<std::int16_t>(kVelOne + kVelOne / 2);
        f.world->wake(x, 75);
    }
    const auto s0 = f.spread(f.id("sand"));
    f.run(120);
    const auto s1 = f.spread(f.id("sand"));
    CHECK_EQ(s1.n, s0.n);
    CHECK(s1.x1 > s0.x1);                        // it slid out of the pile
    CHECK(std::abs(s1.my - s0.my) < 0.5);        // nothing fell
    f.run(60);
    f.sim->tick();
    CHECK_EQ(f.sim->stats().activeChunks, 0);    // and stopped
}

TEST_CASE(topdown_gas_pixels_do_not_rise) {
    Flat side(3, 3), top(3, 3);
    top.flatAll();
    for (Flat* f : {&side, &top}) f->fill("lightgas", 86, 86, 106, 106);
    const auto s0 = top.spread(top.id("lightgas"));
    side.run(150);
    top.run(150);
    const auto s1 = side.spread(side.id("lightgas")), t1 = top.spread(top.id("lightgas"));
    CHECK_EQ(t1.n, s0.n);
    std::printf("  gas mean y: start %.1f, side %.1f, top-down %.1f; top-down box %d..%d\n", s0.my, s1.my, t1.my, t1.y0, t1.y1);
    CHECK(s1.my < s0.my - 30.0);                 // side view: it rose
    CHECK(std::abs(t1.my - s0.my) < 6.0);        // from above: it spread without rising
    CHECK(t1.y1 - t1.y0 > s0.y1 - s0.y0 + 10);
}

TEST_CASE(topdown_gas_grid_has_no_buoyancy) {
    // a light gas on the grid in a closed box: side view it rises, from above it only spreads
    auto meanRow = [](bool flat, double& row0, long long& mass0, long long& mass1) {
        Flat f(2, 2, 2);
        if (flat) f.flatAll();
        GasGrid gas(*f.world, f.mats, f.pool.get(), 5);
        f.sim->setGasExchange(&gas);
        f.sim->params().ambientRate = 0.0f;
        f.fill("stone", 4, 4, 123, 7);
        f.fill("stone", 4, 120, 123, 123);
        f.fill("stone", 4, 4, 7, 123);
        f.fill("stone", 120, 4, 123, 123);
        f.fill("lightgas", 40, 50, 80, 78); // round the box's middle: spreading alone keeps its mean row
        auto row = [&]() {
            const GasFields& g = gas.fields();
            double s = 0, n = 0;
            for (int j = 0; j < g.h; ++j)
                for (int i = 0; i < g.w; ++i)
                    for (int k = 0; k < g.species; ++k) {
                        const double m = static_cast<double>(g.m(k, j * g.w + i));
                        s += m * j;
                        n += m;
                    }
            return n > 0 ? s / n : 0.0;
        };
        f.sim->tick();
        gas.step();
        row0 = row();
        mass0 = f.world->totalMass(f.mats) * kGasMassOne + gas.massFixed();
        for (int t = 0; t < 500; ++t) {
            f.sim->tick();
            gas.step();
        }
        mass1 = f.world->totalMass(f.mats) * kGasMassOne + gas.massFixed();
        return row();
    };
    double s0 = 0, t0 = 0;
    long long sm0 = 0, sm1 = 0, tm0 = 0, tm1 = 0;
    const double side = meanRow(false, s0, sm0, sm1), top = meanRow(true, t0, tm0, tm1);
    std::printf("  grid gas mean row: side view %.2f -> %.2f, top-down %.2f -> %.2f\n", s0, side, t0, top);
    CHECK(side < s0 - 2.5);                      // rose by more than 10 px
    CHECK(std::abs(top - t0) < 0.5);             // stayed where it was
    CHECK_EQ(sm1, sm0);
    CHECK_EQ(tm1, tm0);
}

TEST_CASE(topdown_region_keeps_mass_and_is_thread_count_independent) {
    // half the world seen from above, half side view, with everything moving across the border
    auto runWith = [](int threads, std::uint64_t seed, long long* m0, long long* m1) {
        Flat f(6, 4, threads, seed);
        f.world->gravity().set({{0, 0, 191, 255, 0, 0}, {40, 200, 140, 240, 0, -20}});
        scenes::churn(*f.world, f.mats, 99);
        f.fill("water", 150, 20, 230, 60);
        f.fill("sand", 20, 100, 60, 140);
        f.fill("lightgas", 100, 180, 140, 200);
        if (m0) *m0 = f.world->totalMass(f.mats);
        f.run(300);
        if (m1) *m1 = f.world->totalMass(f.mats);
        return f.world->hash();
    };
    long long m0 = 0, m1 = 0;
    const std::uint64_t one = runWith(1, 42, &m0, &m1);
    CHECK_EQ(m1, m0);
    CHECK(one == runWith(8, 42, nullptr, nullptr));
    CHECK(one == runWith(3, 42, nullptr, nullptr));
    CHECK(one != runWith(8, 43, nullptr, nullptr));
}

TEST_CASE(topdown_player_walks_eight_ways_and_shoves_sand) {
    Flat f(4, 4);
    f.flatAll();
    f.fill("sand", 120, 90, 150, 140);
    Player p(*f.world, f.mats, 1);
    p.inventory().reset(f.mats.size());
    p.spawn(60.5f, 120.0f);
    PlayerInput in;
    const long long m0 = f.world->totalMass(f.mats);
    // north-east for a second: no gravity, both axes move
    in.move = 1.0f;
    in.moveY = -1.0f;
    for (int t = 0; t < 40; ++t) { f.sim->tick(); p.step(in); }
    CHECK(p.topDown());
    CHECK(p.x() > 80.0f);
    CHECK(p.y() < 100.0f);
    // then east, straight through the pile: it's pushed aside, not climbed
    in.moveY = 1.0f;
    for (int t = 0; t < 20; ++t) { f.sim->tick(); p.step(in); }
    in.moveY = 0.0f;
    const float y0 = p.y();
    for (int t = 0; t < 100; ++t) { f.sim->tick(); p.step(in); }
    CHECK(p.x() > 150.0f);                       // walked through the sand
    CHECK(std::abs(p.y() - y0) < 3.0f);          // without stepping up or falling
    CHECK_EQ(f.world->totalMass(f.mats), m0);    // grains moved, none lost
    // jumping does nothing from above
    in.move = 0.0f;
    in.jump = true;
    const float y1 = p.y();
    for (int t = 0; t < 30; ++t) { f.sim->tick(); p.step(in); }
    CHECK(std::abs(p.y() - y1) < 1.0f);
}
