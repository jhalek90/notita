// Unit tests for the cellular automaton rules, chunking and determinism.
#include "test.h"

#include "sim/Brush.h"
#include "sim/Material.h"
#include "sim/Scenes.h"
#include "sim/Simulator.h"
#include "sim/ThreadPool.h"
#include "sim/World.h"

#include <algorithm>
#include <memory>

using namespace notita;

namespace {

// Test materials are defined here, not taken from data/materials.txt, so
// tuning the game data never breaks the rule tests.
const char* kTestMaterials = R"(
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

[syrup]
kind = liquid
density = 1400
viscosity = 0.9
spread = 2
color = #c06010

[metal]
kind = liquid
density = 13000
spread = 4
color = #c0c8d0

[lightgas]
kind = gas
density = 0.5
spread = 2
color = #e0e0e0

[heavygas]
kind = gas
density = 4
spread = 2
color = #608040
)";

struct Fixture {
    MaterialRegistry mats;
    std::unique_ptr<World> world;
    std::unique_ptr<ThreadPool> pool;
    std::unique_ptr<Simulator> sim;
    Rng rng{7};

    Fixture(int chunksW, int chunksH, int threads = 4, std::uint64_t seed = 1) {
        std::vector<std::string> errors;
        mats.loadFromString(kTestMaterials, &errors);
        CHECK(errors.empty());
        world = std::make_unique<World>(0, 0, chunksW, chunksH);
        pool = std::make_unique<ThreadPool>(threads);
        sim = std::make_unique<Simulator>(*world, mats, pool.get(), seed);
    }
    MaterialId id(const char* n) const { return mats.idOf(n); }
    void fill(const char* m, int x0, int y0, int x1, int y1) { scenes::fillRect(*world, id(m), x0, y0, x1, y1, rng); }
    void run(int ticks) { for (int i = 0; i < ticks; ++i) sim->tick(); }
    MaterialId at(int x, int y) const { return world->material(x, y); }
    double meanY(MaterialId m) const {
        double sum = 0; std::size_t n = 0;
        for (int y = 0; y < world->heightPx(); ++y)
            for (int x = 0; x < world->widthPx(); ++x)
                if (at(x, y) == m) { sum += y; ++n; }
        return n ? sum / static_cast<double>(n) : 0.0;
    }
};

} // namespace

TEST_CASE(materials_parse) {
    MaterialRegistry reg;
    std::vector<std::string> errors;
    const bool ok = reg.loadFromString(R"(
# comment line
[glow_sand]   ; trailing comment
kind = powder
density = 1500
color = #ff8000   # orange
color_variation = 20
melts_into = glass   # unknown keys are kept for later phases

[thin]
kind = liquid
density = 900
viscosity = 2.0
spread = 99
color = #11223344
)", &errors);
    CHECK(ok);
    CHECK(errors.empty());
    CHECK_EQ(reg.size(), std::size_t{3});
    const Material* g = reg.find("glow_sand");
    CHECK(g != nullptr);
    if (g) {
        CHECK(g->kind == MaterialKind::Powder);
        CHECK(g->density == 1500.0f);
        CHECK_EQ(g->color, 0xFF0080FFu);          // RGBA8 memory order
        CHECK(g->extra.count("melts_into") == 1 && g->extra.at("melts_into") == "glass");
        CHECK(g->label == "Glow sand");
    }
    const Material* t = reg.find("thin");
    CHECK(t != nullptr);
    if (t) {
        CHECK(t->viscosity == 1.0f);  // clamped
        CHECK_EQ(t->spread, 16);      // clamped to the checkerboard margin
        CHECK_EQ(t->color, 0x44332211u);
    }
    CHECK_EQ(reg.idOf("air"), kEmpty);
    CHECK_EQ(reg.idOf("nope"), kEmpty);

    // errors are reported, good sections still load
    MaterialRegistry bad;
    errors.clear();
    CHECK(!bad.loadFromString("[a]\nkind = plasma\n[b]\nkind = solid\ndensity = x\n[c]\nkind = solid\n", &errors));
    CHECK_EQ(errors.size(), std::size_t{3}); // bad kind, a has no kind, bad density
    CHECK(bad.find("c") != nullptr);
}

TEST_CASE(materials_default_data_loads) {
    std::vector<std::string> errors;
    MaterialRegistry reg = MaterialRegistry::loadDefault(&errors);
    CHECK(errors.empty());
    CHECK(reg.find("sand") != nullptr);
    CHECK(reg.find("water") != nullptr);
    CHECK(reg.size() >= 8);
}

TEST_CASE(sand_falls_and_rests_on_solid) {
    Fixture f(2, 2);
    f.fill("stone", 0, 100, 127, 100);
    f.world->place(10, 10, f.id("sand"), 0);
    f.run(200);
    CHECK_EQ(f.at(10, 99), f.id("sand"));
    CHECK_EQ(f.world->count(f.id("sand")), std::size_t{1});
}

TEST_CASE(sand_crosses_chunk_boundaries) {
    Fixture f(2, 2);
    // starts in chunk (1,0), the world bottom (y = 127) is the edge of chunk (1,1)
    f.world->place(70, 5, f.id("sand"), 0);
    f.run(200);
    CHECK_EQ(f.at(70, 127), f.id("sand"));
}

TEST_CASE(sand_forms_a_pile) {
    Fixture f(4, 2);
    f.fill("sand", 120, 0, 130, 60);   // 11 x 61 column
    f.run(1500);
    int height = 0, left = 1 << 30, right = -1;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 256; ++x)
            if (f.at(x, y) == f.id("sand")) {
                height = std::max(height, 128 - y);
                left = std::min(left, x);
                right = std::max(right, x);
            }
    const int width = right - left + 1;
    CHECK(height < 61);           // it slumped
    CHECK(width > 30);            // and spread into a pile
    CHECK(height > 6);            // but did not flow flat like a liquid
    CHECK_EQ(f.world->count(f.id("sand")), std::size_t{11 * 61});
}

TEST_CASE(water_finds_its_level) {
    Fixture f(2, 2);
    // U-shaped basin: walls at x = 20 and x = 100, floor at y = 120
    f.fill("stone", 20, 60, 20, 120);
    f.fill("stone", 100, 60, 100, 120);
    f.fill("stone", 20, 120, 100, 120);
    f.fill("water", 22, 70, 40, 119);  // tall column against the left wall
    f.run(1500);
    int lo = 1 << 30, hi = -1;
    for (int x = 21; x < 100; ++x) {
        int top = 120;
        while (top > 60 && f.at(x, top - 1) == f.id("water")) --top;
        lo = std::min(lo, top);
        hi = std::max(hi, top);
    }
    CHECK(hi - lo <= 1); // flat surface across the whole basin
    CHECK_EQ(f.world->count(f.id("water")), std::size_t{19 * 50});
}

TEST_CASE(density_sorting_oil_floats_on_water) {
    Fixture f(2, 2);
    // oil starts *under* water in a closed column
    f.fill("stone", 30, 20, 30, 120);
    f.fill("stone", 60, 20, 60, 120);
    f.fill("stone", 30, 121, 60, 121);
    f.fill("oil", 31, 90, 59, 120);
    f.fill("water", 31, 60, 59, 89);
    f.run(2500);
    int misplaced = 0; // water pixels with oil anywhere below them
    for (int x = 31; x < 60; ++x) {
        bool oilBelow = false;
        for (int y = 120; y >= 20; --y) {
            if (f.at(x, y) == f.id("oil")) oilBelow = true;
            if (f.at(x, y) == f.id("water") && oilBelow) ++misplaced;
        }
    }
    CHECK(misplaced <= 3);
    CHECK(f.meanY(f.id("oil")) < f.meanY(f.id("water")));
}

TEST_CASE(density_sorting_powder_and_heavy_liquid_sink) {
    Fixture f(2, 2);
    f.fill("stone", 30, 20, 30, 120);
    f.fill("stone", 60, 20, 60, 120);
    f.fill("stone", 30, 121, 60, 121);
    f.fill("water", 31, 70, 59, 120);
    f.fill("sand", 31, 55, 59, 60);
    f.fill("metal", 40, 40, 45, 45);
    f.run(2500);
    // metal (13000) percolates to the very bottom, then sand (1600), then water (1000)
    CHECK(f.meanY(f.id("metal")) > f.meanY(f.id("sand")));
    CHECK(f.meanY(f.id("sand")) > f.meanY(f.id("water")));
    int metalOnFloor = 0;
    for (int x = 31; x < 60; ++x) metalOnFloor += f.at(x, 120) == f.id("metal");
    CHECK(metalOnFloor >= 5); // it can't flow sideways under the sand, so it stays a clump
}

TEST_CASE(light_powder_floats_on_heavy_liquid) {
    Fixture f(2, 2);
    f.fill("stone", 30, 20, 30, 120);
    f.fill("stone", 60, 20, 60, 120);
    f.fill("stone", 30, 121, 60, 121);
    f.fill("metal", 31, 100, 59, 120);
    f.fill("sand", 31, 60, 59, 65);
    f.run(1500);
    CHECK(f.meanY(f.id("sand")) < f.meanY(f.id("metal")));
    for (int x = 31; x < 60; ++x) CHECK_EQ(f.at(x, 120), f.id("metal"));
}

TEST_CASE(gases_rise_or_sink_by_density) {
    Fixture f(2, 2);
    f.fill("lightgas", 40, 60, 50, 70);
    f.fill("heavygas", 80, 60, 90, 70);
    const double light0 = f.meanY(f.id("lightgas")), heavy0 = f.meanY(f.id("heavygas"));
    f.run(300);
    CHECK(f.meanY(f.id("lightgas")) < light0 - 30);
    CHECK(f.meanY(f.id("heavygas")) > heavy0 + 30);
    CHECK_EQ(f.world->count(f.id("lightgas")), std::size_t{121});
    CHECK_EQ(f.world->count(f.id("heavygas")), std::size_t{121});
}

TEST_CASE(viscosity_slows_spreading) {
    Fixture f(4, 2);
    f.fill("water", 60, 100, 64, 127);
    f.fill("syrup", 190, 100, 194, 127);
    f.run(90);
    auto extent = [&](MaterialId m, int x0, int x1) {
        int l = 1 << 30, r = -1;
        for (int y = 0; y < 128; ++y)
            for (int x = x0; x < x1; ++x)
                if (f.at(x, y) == m) { l = std::min(l, x); r = std::max(r, x); }
        return r - l + 1;
    };
    const int water = extent(f.id("water"), 0, 128);
    const int syrup = extent(f.id("syrup"), 128, 256);
    CHECK(water > syrup + 20);
}

TEST_CASE(mass_is_conserved) {
    Fixture f(5, 3);
    scenes::churn(*f.world, f.mats, 99);
    std::vector<std::size_t> before;
    for (std::size_t m = 0; m < f.mats.size(); ++m) before.push_back(f.world->count(static_cast<MaterialId>(m)));
    f.run(400);
    for (std::size_t m = 0; m < f.mats.size(); ++m) CHECK_EQ(f.world->count(static_cast<MaterialId>(m)), before[m]);
}

TEST_CASE(determinism_independent_of_thread_count) {
    auto runWith = [](int threads, std::uint64_t seed) {
        Fixture f(6, 4, threads, seed);
        scenes::churn(*f.world, f.mats, 1234);
        Rng brushRng(5);
        Brush b{f.id("water"), 6, BrushShape::Circle, BrushMode::Replace, 1.0f};
        for (int t = 0; t < 300; ++t) {
            if (t % 50 == 0) strokeBrush(*f.world, f.mats, b, 20, 20, 300, 40, brushRng);
            f.sim->tick();
        }
        return f.world->hash();
    };
    const std::uint64_t one = runWith(1, 42);
    const std::uint64_t many = runWith(8, 42);
    const std::uint64_t again = runWith(3, 42);
    const std::uint64_t other = runWith(8, 43);
    CHECK(one == many);
    CHECK(one == again);
    CHECK(one != other);
}

TEST_CASE(settled_world_goes_to_sleep) {
    Fixture f(3, 2);
    f.fill("sand", 20, 10, 60, 40);
    // basin filled exactly to the brim with water: nowhere to flow, so it sleeps too
    f.fill("stone", 120, 100, 120, 127);
    f.fill("stone", 150, 100, 150, 127);
    f.fill("water", 121, 100, 149, 127);
    f.run(1500);
    f.sim->tick();
    CHECK_EQ(f.sim->stats().activeChunks, 0);
    // poking it wakes only what is around the change
    f.world->place(130, 10, f.id("sand"), 0);
    f.sim->tick();
    CHECK(f.sim->stats().activeChunks >= 1);
    CHECK(f.sim->stats().activeChunks <= 2);
}

TEST_CASE(brush_place_replace_erase) {
    Fixture f(2, 2);
    f.fill("stone", 0, 50, 127, 50);
    Rng rng(1);
    Brush b{f.id("sand"), 3, BrushShape::Square, BrushMode::Place, 1.0f};
    applyBrush(*f.world, f.mats, b, 20, 50, rng);   // square 7x7 centred on the stone line
    CHECK_EQ(f.world->count(f.id("sand")), std::size_t{42}); // the 7 stone cells are kept
    CHECK_EQ(f.at(20, 50), f.id("stone"));
    b.mode = BrushMode::Replace;
    applyBrush(*f.world, f.mats, b, 20, 50, rng);
    CHECK_EQ(f.world->count(f.id("sand")), std::size_t{49});
    b.mode = BrushMode::Erase;
    b.shape = BrushShape::Circle;
    b.radius = 10;
    applyBrush(*f.world, f.mats, b, 20, 50, rng);
    CHECK_EQ(f.world->count(f.id("sand")), std::size_t{0});
    // strokes leave no gaps
    b = Brush{f.id("stone"), 0, BrushShape::Square, BrushMode::Replace, 1.0f};
    strokeBrush(*f.world, f.mats, b, 0, 10, 100, 30, rng);
    for (int x = 0; x <= 100; ++x) {
        bool any = false;
        for (int y = 0; y < 40; ++y) any |= f.at(x, y) == f.id("stone");
        CHECK(any);
    }
}

TEST_CASE(sand_wall_collapses_instead_of_standing) {
    // A 71x100 block on a ledge must slump into a cone. Before the
    // "fresh hole" rule its faces stayed vertical down to y = 124 (a bell).
    Fixture f(4, 3);
    f.fill("stone", 30, 150, 170, 155);
    f.fill("sand", 60, 50, 130, 149);
    f.run(400);
    auto edges = [&](int y, int& l, int& r) {
        l = -1; r = -1;
        for (int x = 0; x < 256; ++x) if (f.at(x, y) == f.id("sand")) { if (l < 0) l = x; r = x; }
    };
    int l, r;
    edges(66, l, r);
    CHECK(r - l < 40);        // narrow near the top
    edges(122, l, r);
    CHECK(l < 52 && r > 138); // faces moved out at two-thirds height
    edges(146, l, r);
    CHECK(l < 40 && r > 150); // wide base
}
TEST_CASE(water_slides_under_oil) {
    // side by side in a closed box: water must end up under the oil
    Fixture f(2, 2);
    f.fill("stone", 20, 40, 20, 120);
    f.fill("stone", 100, 40, 100, 120);
    f.fill("stone", 20, 121, 100, 121);
    f.fill("oil", 21, 60, 60, 120);
    f.fill("water", 61, 60, 99, 120);
    f.run(2000);
    int misplaced = 0; // water pixels with oil anywhere below them
    for (int x = 21; x < 100; ++x) {
        bool oilBelow = false;
        for (int y = 120; y >= 40; --y) {
            if (f.at(x, y) == f.id("oil")) oilBelow = true;
            if (f.at(x, y) == f.id("water") && oilBelow) ++misplaced;
        }
    }
    int lo = 1 << 30, hi = -1; // the oil/water interface should be (nearly) flat
    for (int x = 21; x < 100; ++x) {
        int y = 40;
        while (y <= 120 && f.at(x, y) != f.id("water")) ++y;
        lo = std::min(lo, y); hi = std::max(hi, y);
    }
    CHECK(misplaced <= 10);
    CHECK(hi - lo <= 8);
}
