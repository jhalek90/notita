// Phase 2 tests: heat conduction, state changes, fire, the Luau reaction
// table, mass conservation and the phase 3 gas hook.
#include "test.h"

#include "script/ReactionScript.h"
#include "sim/Brush.h"
#include "sim/GasExchange.h"
#include "sim/Material.h"
#include "sim/Reactions.h"
#include "sim/Scenes.h"
#include "sim/Simulator.h"
#include "sim/ThreadPool.h"
#include "sim/World.h"

#include <chrono>
#include <cmath>
#include <memory>

using namespace notita;

namespace {

// Own material table (like test_sim.cpp) so retuning data/ doesn't break these.
const char* kHeatMaterials = R"(
[stone]
kind = solid
density = 2600
conductivity = 0.2
heat_capacity = 1.0
color = #808080

[metal]
kind = solid
density = 7800
conductivity = 0.9
heat_capacity = 0.5
color = #a0a8b0

[felt]
kind = solid
density = 300
conductivity = 0.02
heat_capacity = 1.0
color = #605040

[water]
kind = liquid
density = 1000
spread = 6
conductivity = 0.3
heat_capacity = 1.0
boil_point = 100
boils_into = steam
latent_heat = 8
freeze_point = 0
freezes_into = ice
color = #3070c0

[steam]
kind = gas
density = 0.6
spread = 2
conductivity = 0.05
heat_capacity = 0.5
condense_point = 70
condenses_into = water
color = #d0d8e0

[ice]
kind = solid
density = 917
conductivity = 0.35
heat_capacity = 0.9
melt_point = 2
melts_into = water
color = #c0e0f0

[wood]
kind = solid
density = 600
conductivity = 0.05
heat_capacity = 1.2
ignite_point = 300
burn_time = 60
burn_heat = 16
flame_chance = 0.3
burns_into = smoke
burn_residue = ash
residue_chance = 0.5
color = #7b5232

[ash]
kind = powder
density = 500
color = #888888

[smoke]
kind = gas
density = 0.9
spread = 2
color = #404040

[flame]
kind = special
density = 0.3
conductivity = 0.6
heat_capacity = 0.3
spawn_temp = 950
life_min = 10
life_max = 20
color = #ff9a2a

[acid]
kind = liquid
density = 1150
spread = 5
color = #90e040

[plate]
kind = solid
density = 7000
color = #909090

[salt]
kind = powder
density = 1900
color = #b06030

[fizz]
kind = gas
density = 0.1
spread = 3
color = #e0e8ff
)";

const char* kHeatReactions = R"(
local function r(a, b, pa, pb, heat, chance, extra)
    local e = { a = a, b = b, into = { pa, pb }, heat = heat, chance = chance }
    if extra then
        for k, v in extra do e[k] = v end
    end
    return e
end
return {
    r("acid", "plate", "salt", "fizz", 50, 0.2),
    r("flame", "water", "air", "steam", -60, 0.6),
    r("ice", "salt", "water", "salt", -4, 0.05, { min_temp = -50 }),
}
)";

struct HeatFixture {
    MaterialRegistry mats;
    std::unique_ptr<ReactionTable> reactions;
    std::unique_ptr<World> world;
    std::unique_ptr<ThreadPool> pool;
    std::unique_ptr<Simulator> sim;
    Rng rng{11};

    HeatFixture(int chunksW, int chunksH, int threads = 4, std::uint64_t seed = 1) {
        std::vector<std::string> errors;
        mats.loadFromString(kHeatMaterials, &errors);
        CHECK(errors.empty());
        for (auto& e : errors) std::printf("    %s\n", e.c_str());
        reactions = std::make_unique<ReactionTable>(mats);
        CHECK(loadReactions(*reactions, kHeatReactions, "test_reactions", &errors));
        for (auto& e : errors) std::printf("    %s\n", e.c_str());
        world = std::make_unique<World>(0, 0, chunksW, chunksH);
        pool = std::make_unique<ThreadPool>(threads);
        sim = std::make_unique<Simulator>(*world, mats, pool.get(), seed, reactions.get());
    }
    MaterialId id(const char* n) const { return mats.idOf(n); }
    void fill(const char* m, int x0, int y0, int x1, int y1, float t = kAmbientTemp) {
        scenes::fillRect(*world, id(m), x0, y0, x1, y1, rng, t);
    }
    // stone walls around an inclusive interior rect
    void box(int x0, int y0, int x1, int y1, float t = kAmbientTemp) {
        fill("stone", x0 - 1, y0 - 1, x1 + 1, y0 - 1, t);
        fill("stone", x0 - 1, y1 + 1, x1 + 1, y1 + 1, t);
        fill("stone", x0 - 1, y0, x0 - 1, y1, t);
        fill("stone", x1 + 1, y0, x1 + 1, y1, t);
    }
    void run(int ticks) { for (int i = 0; i < ticks; ++i) sim->tick(); }
    float temp(int x, int y) const { return world->cell(x, y)->temperature; }
    std::size_t count(const char* m) const { return world->count(id(m)); }
    void setAllTemps(float t) {
        for (Chunk* c : world->chunks())
            for (Cell& cell : c->cells) cell.temperature = t;
        world->wakeAll();
    }
};

} // namespace

TEST_CASE(heat_conduction_conserves_energy_across_chunks) {
    HeatFixture f(2, 2);
    f.sim->params().ambientRate = 0.0f;
    f.fill("stone", 0, 0, 63, 127, 500.0f);   // left chunks hot
    f.fill("stone", 64, 0, 127, 127, 20.0f);  // right chunks cold: heat has to cross the chunk edge
    const double e0 = f.world->thermalEnergy(f.mats);
    f.run(600);
    const double e1 = f.world->thermalEnergy(f.mats);
    CHECK(std::abs(e1 - e0) / e0 < 1e-4);
    CHECK(f.temp(70, 40) > 60.0f);   // right side warmed up
    CHECK(f.temp(57, 40) < 460.0f);  // left side cooled down
    CHECK(f.temp(57, 40) > f.temp(70, 40)); // and nothing overshot
    for (const Chunk* c : f.world->chunks())
        for (const Cell& cell : c->cells) CHECK(cell.temperature >= 20.0f - 1e-3f && cell.temperature <= 500.0f + 1e-3f);
}

TEST_CASE(heat_wakes_a_sleeping_neighbour_chunk) {
    HeatFixture f(3, 1);
    f.fill("stone", 0, 0, 191, 63);
    f.run(5);
    f.sim->tick();
    CHECK_EQ(f.sim->stats().heatChunks, 0); // uniform temperature: heat sleeps
    // heat the middle chunk's left edge and wake only the middle chunk
    for (int y = 0; y < 64; ++y) {
        f.world->cell(64, y)->temperature = 900.0f;
        f.world->cell(65, y)->temperature = 900.0f;
    }
    f.world->wakeRect(70, 10, 80, 20);
    f.run(200);
    CHECK(f.temp(62, 30) > 40.0f);  // crossed into the left chunk, which was asleep
    CHECK(f.temp(67, 30) > 40.0f);
}

TEST_CASE(heat_conductivity_sets_the_speed) {
    auto farEnd = [](const char* m) {
        HeatFixture f(1, 1);
        f.sim->params().ambientRate = 0.0f;
        f.fill("felt", 0, 0, 63, 63);
        f.fill(m, 10, 30, 50, 32);
        f.fill(m, 10, 30, 12, 32, 800.0f);
        f.run(120);
        return f.temp(18, 31); // 6 px down the bar
    };
    const float metal = farEnd("metal"), felt = farEnd("felt");
    CHECK(metal > felt + 20.0f);
    CHECK(felt < 25.0f);
}

TEST_CASE(water_boils_into_matching_steam_and_condenses_back) {
    HeatFixture f(1, 1);
    f.sim->params().ambientRate = 0.0f;
    f.box(10, 10, 41, 41, 200.0f);
    f.fill("water", 10, 32, 41, 41, 200.0f); // 32 x 10 = 320 cells
    const long long mass0 = f.world->totalMass(f.mats);
    CHECK_EQ(mass0 - 4 * 33, 320); // walls + water (air is massless)
    bool conserved = true;
    for (int t = 0; t < 400; ++t) {
        f.sim->tick();
        conserved = conserved && f.world->totalMass(f.mats) == mass0;
    }
    CHECK(conserved);
    CHECK_EQ(f.count("water"), std::size_t{0});
    CHECK_EQ(f.count("steam"), std::size_t{320}); // one steam pixel per water pixel
    // the steam is less dense than water: it fills the box from the top
    // cool everything: the steam condenses back into exactly as much water
    f.setAllTemps(20.0f);
    for (int t = 0; t < 600; ++t) {
        f.sim->tick();
        conserved = conserved && f.world->totalMass(f.mats) == mass0;
    }
    CHECK(conserved);
    CHECK_EQ(f.count("steam"), std::size_t{0});
    CHECK_EQ(f.count("water"), std::size_t{320});
    CHECK(f.temp(25, 41) > 20.0f); // the latent heat came back on condensing
}

TEST_CASE(water_freezes_and_ice_melts) {
    HeatFixture f(1, 1);
    f.sim->params().ambientRate = 0.0f;
    f.box(5, 40, 50, 60, -20.0f);
    f.fill("water", 5, 50, 50, 60, -20.0f);
    const std::size_t n = f.count("water");
    f.run(60);
    CHECK_EQ(f.count("water"), std::size_t{0});
    CHECK_EQ(f.count("ice"), n);
    f.setAllTemps(30.0f);
    f.run(60);
    CHECK_EQ(f.count("ice"), std::size_t{0});
    CHECK_EQ(f.count("water"), n);
}

TEST_CASE(fire_spreads_through_wood_and_keeps_mass) {
    HeatFixture f(2, 2);
    f.fill("stone", 0, 120, 127, 127);
    f.fill("wood", 40, 100, 79, 119);              // 40 x 20 = 800 cells
    f.fill("wood", 40, 100, 42, 119, 650.0f);      // light one edge
    const long long mass0 = f.world->totalMass(f.mats);
    bool conserved = true, sawFlames = false;
    for (int t = 0; t < 2500; ++t) {
        f.sim->tick();
        conserved = conserved && f.world->totalMass(f.mats) == mass0;
        sawFlames = sawFlames || f.count("flame") > 0;
    }
    CHECK(conserved);
    CHECK(sawFlames);
    const std::size_t wood = f.count("wood"), smoke = f.count("smoke"), ash = f.count("ash");
    CHECK(wood < 100);                         // the fire ate through the block
    CHECK(smoke > 0);
    CHECK(ash > 0);
    CHECK_EQ(wood + smoke + ash, std::size_t{800}); // every unit of wood ended up somewhere
}

TEST_CASE(fire_needs_air) {
    HeatFixture f(1, 1);
    f.fill("stone", 0, 0, 63, 63);
    f.fill("wood", 20, 20, 40, 40, 650.0f); // buried in stone: no air to burn with
    f.run(300);
    CHECK_EQ(f.count("wood"), std::size_t{21 * 21});
    CHECK_EQ(f.count("flame"), std::size_t{0});
}

TEST_CASE(luau_reaction_table_loads_and_reports_bad_entries) {
    MaterialRegistry mats;
    mats.loadFromString(kHeatMaterials);
    ReactionTable table(mats);
    std::vector<std::string> errors;
    CHECK(loadReactions(table, kHeatReactions, "ok", &errors));
    CHECK(errors.empty());
    CHECK_EQ(table.size(), std::size_t{3});
    const Reaction& r = table.reactions()[0];
    CHECK_EQ(r.a, mats.idOf("acid"));
    CHECK_EQ(r.outB, mats.idOf("fizz"));
    CHECK(r.heat == 50.0f);
    CHECK(std::abs(r.chance - 0.2f) < 1e-6f);
    CHECK(table.reactive(mats.idOf("plate")));
    CHECK(!table.reactive(mats.idOf("stone")));
    CHECK(table.reactions()[2].minTemp == -50.0f);

    ReactionTable bad(mats);
    errors.clear();
    CHECK(!loadReactions(bad, R"(
return {
    { a = "acid", b = "plate", into = { "salt", "fizz" } },       -- fine
    { a = "acid", b = "unobtainium", into = { "salt" } },         -- unknown material
    { a = "acid", b = "stone", into = { "salt" } },               -- 2 mass units -> 1
    { a = "acid", b = "metal", into = { "salt", "fizz" }, heat = "lots" }, -- bad type
    "not a table",
    { a = "acid", b = "plate", into = { "salt", "fizz" } },       -- duplicate pair
}
)", "bad", &errors));
    CHECK_EQ(bad.size(), std::size_t{1});
    CHECK_EQ(errors.size(), std::size_t{5});
    for (auto& e : errors) std::printf("    (expected) %s\n", e.c_str());

    errors.clear();
    CHECK(!loadReactions(bad, "return {", "syntax", &errors));
    CHECK_EQ(errors.size(), std::size_t{1});
    errors.clear();
    CHECK(!loadReactions(bad, "return 42", "nottable", &errors));
    CHECK_EQ(errors.size(), std::size_t{1});
}

TEST_CASE(luau_sandbox_blocks_escapes_and_runaway_scripts) {
    const char* escapes[] = {
        "os.execute('echo hi') return {}",
        "io.open('x.txt', 'w') return {}",
        "local f = loadstring('return 1') return {}",
        "require('something') return {}",
        "print('hello') return {}",
        "getfenv(0) return {}",
        "string.rep = nil return {}",      // libraries are read-only
        "debug.traceback() return {}",
    };
    for (const char* src : escapes) {
        std::vector<std::string> errors;
        const auto specs = runReactionScript(src, "escape", &errors);
        CHECK(specs.empty());
        CHECK_EQ(errors.size(), std::size_t{1});
    }
    // an endless loop is stopped by the instruction budget
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> errors;
    CHECK(runReactionScript("while true do end return {}", "loop", &errors).empty());
    CHECK_EQ(errors.size(), std::size_t{1});
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
    // and a memory hog by the memory limit
    errors.clear();
    ScriptLimits small;
    small.memoryBytes = 1u << 20;
    small.interrupts = 100'000'000;
    CHECK(runReactionScript("local t = {} for i = 1, 10000000 do t[i] = i end return {}", "hog", &errors, small).empty());
    CHECK_EQ(errors.size(), std::size_t{1});
    // ordinary globals in the script's own environment are fine
    errors.clear();
    CHECK_EQ(runReactionScript("x = 3 return { { a = 'acid', b = 'plate', into = 'salt', chance = x / 10 } }", "globals", &errors).size(),
             std::size_t{1});
    CHECK(errors.empty());
}

TEST_CASE(reaction_makes_products_heat_and_keeps_mass) {
    HeatFixture f(1, 1);
    f.sim->params().ambientRate = 0.0f;
    f.box(10, 10, 50, 50);
    f.fill("plate", 10, 46, 50, 50);
    f.fill("acid", 10, 30, 50, 45);
    const long long mass0 = f.world->totalMass(f.mats);
    const double e0 = f.world->thermalEnergy(f.mats);
    bool conserved = true;
    for (int t = 0; t < 300; ++t) {
        f.sim->tick();
        conserved = conserved && f.world->totalMass(f.mats) == mass0;
    }
    CHECK(conserved);
    CHECK(f.count("salt") > 20);
    CHECK_EQ(f.count("salt"), f.count("fizz")); // one of each per reaction (nothing leaves the box)
    CHECK(f.world->thermalEnergy(f.mats) > e0); // exothermic
}

TEST_CASE(reaction_temperature_condition) {
    HeatFixture f(1, 1);
    f.sim->params().ambientRate = 0.0f;
    f.fill("stone", 0, 0, 63, 63, -80.0f);
    f.fill("ice", 10, 10, 30, 20, -80.0f);
    f.fill("salt", 10, 21, 30, 22, -80.0f);
    f.run(200);
    CHECK_EQ(f.count("water"), std::size_t{0}); // too cold (min_temp -50)
    f.setAllTemps(-30.0f);
    f.run(200);
    CHECK(f.count("water") > 0);                 // warm enough: the salt eats the ice
}

TEST_CASE(heat_determinism_independent_of_thread_count) {
    auto runWith = [](int threads, std::uint64_t seed) {
        HeatFixture f(6, 4, threads, seed);
        scenes::churn(*f.world, f.mats, 1234); // test table: water, steam, ... at 20 C
        // hot and cold patches, burning wood, acid on plate
        f.fill("wood", 40, 120, 120, 160);
        f.fill("wood", 40, 120, 50, 160, 700.0f);
        f.fill("plate", 200, 200, 260, 210);
        f.fill("acid", 200, 180, 260, 199);
        f.fill("water", 300, 20, 360, 60, 140.0f);
        f.fill("ice", 300, 100, 340, 140, -40.0f);
        Rng brushRng(5);
        Brush heat{kEmpty, 8, BrushShape::Circle, BrushMode::Heat, 1.0f, 80.0f};
        for (int t = 0; t < 300; ++t) {
            if (t % 30 == 0) strokeBrush(*f.world, f.mats, heat, 100, 200, 300, 240, brushRng);
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

TEST_CASE(heat_goes_to_sleep_when_settled) {
    HeatFixture f(2, 2);
    f.sim->params().ambientRate = 0.05f;
    f.fill("stone", 0, 100, 127, 127);
    f.run(3);
    f.sim->tick();
    CHECK_EQ(f.sim->stats().heatChunks, 0);
    CHECK_EQ(f.sim->stats().activeChunks, 0);
    // a warm spot keeps heat awake for a while, then everything sleeps again
    f.fill("stone", 60, 90, 67, 99, 120.0f);
    f.sim->tick();
    CHECK(f.sim->stats().heatChunks > 0);
    int sleptAt = -1;
    for (int t = 0; t < 30000 && sleptAt < 0; ++t) {
        f.sim->tick();
        if (f.sim->stats().heatChunks == 0 && f.sim->stats().activeChunks == 0) sleptAt = t;
    }
    CHECK(sleptAt > 0);
}

namespace {
struct MockGrid : GasExchange {
    MaterialId steam;
    long long mass = 0;
    std::size_t emissions = 0;
    double ambient = 0;
    explicit MockGrid(MaterialId s) : steam(s) {}
    bool takesGas(MaterialId g) const override { return g == steam; }
    void absorb(const GasEmission* e, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i) { mass += e[i].mass; CHECK(e[i].temperature > 70.0f); }
        emissions += n;
    }
    void ambientHeat(double energy) override { ambient += energy; }
};
} // namespace

TEST_CASE(gas_exchange_takes_created_gas_and_ambient_heat) {
    HeatFixture f(1, 1);
    MockGrid grid(f.id("steam"));
    f.sim->setGasExchange(&grid);
    f.box(10, 30, 41, 41, 200.0f);
    f.fill("water", 10, 32, 41, 41, 200.0f); // 320 cells
    f.fill("stone", 0, 0, 5, 5, 20.0f);
    f.world->cell(30, 10)->temperature = 600.0f; // hot air outside the box loses heat to the ambient
    const long long mass0 = f.world->totalMass(f.mats);
    bool conserved = true;
    for (int t = 0; t < 400; ++t) {
        f.sim->tick();
        conserved = conserved && f.world->totalMass(f.mats) + grid.mass == mass0;
    }
    CHECK(conserved);
    CHECK_EQ(grid.mass, 320);
    CHECK_EQ(grid.emissions, std::size_t{320});
    CHECK_EQ(f.count("steam"), std::size_t{0});
    CHECK(grid.ambient > 0.0);
}

TEST_CASE(material_changes_must_keep_mass) {
    MaterialRegistry reg;
    std::vector<std::string> errors;
    CHECK(!reg.loadFromString(R"(
[light]
kind = liquid
mass = 1
boil_point = 50
boils_into = heavy
[heavy]
kind = gas
mass = 2
[orphan]
kind = solid
melt_point = 100
[ghost]
kind = solid
melts_into = nowhere
melt_point = 10
)", &errors));
    CHECK_EQ(errors.size(), std::size_t{3});
    for (auto& e : errors) std::printf("    (expected) %s\n", e.c_str());
    CHECK(!reg.find("light")->hot.valid);
    CHECK(!reg.find("ghost")->hot.valid);
}

TEST_CASE(default_reactions_and_materials_load) {
    std::vector<std::string> errors;
    MaterialRegistry mats = MaterialRegistry::loadDefault(&errors);
    for (auto& e : errors) std::printf("    %s\n", e.c_str());
    CHECK(errors.empty());
    ReactionTable table(mats);
    CHECK(loadDefaultReactions(table, &errors));
    for (auto& e : errors) std::printf("    %s\n", e.c_str());
    CHECK(errors.empty());
    CHECK(table.size() >= 5);
    CHECK(mats.find("water")->hot.valid);
    CHECK(mats.find("steam")->cold.valid);
    CHECK(mats.find("timber")->flammable());
    CHECK(mats.find("flame")->mass == 0);
}

TEST_CASE(brush_heats_and_cools) {
    HeatFixture f(1, 1);
    f.fill("stone", 0, 0, 63, 63);
    Rng rng(3);
    Brush b{kEmpty, 4, BrushShape::Circle, BrushMode::Heat, 1.0f, 50.0f};
    applyBrush(*f.world, f.mats, b, 30, 30, rng);
    CHECK(std::abs(f.temp(30, 30) - 70.0f) < 1e-3f);
    CHECK(f.temp(34, 30) > 20.0f && f.temp(34, 30) < 70.0f); // fades towards the rim
    CHECK(f.temp(40, 30) == 20.0f);
    b.mode = BrushMode::Cool;
    applyBrush(*f.world, f.mats, b, 30, 30, rng);
    applyBrush(*f.world, f.mats, b, 30, 30, rng);
    CHECK(std::abs(f.temp(30, 30) - 20.0f + 50.0f) < 1e-3f);
    // placed material starts at its spawn temperature
    Brush p{f.id("flame"), 0, BrushShape::Circle, BrushMode::Replace, 1.0f};
    applyBrush(*f.world, f.mats, p, 10, 10, rng);
    CHECK(f.temp(10, 10) == 950.0f);
}
