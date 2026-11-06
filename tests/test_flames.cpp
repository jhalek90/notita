// #480 tests: flames as hot reacting gas on the gas grid. Burning pixels hand
// fuel vapour (energy, no mass) to the grid, it burns there, and only embers
// still fly as pixels. Own material table, CPU solver, no GPU.
#include "test.h"

#include "sim/ChunkCodec.h"
#include "sim/GasGrid.h"
#include "sim/Material.h"
#include "sim/Scenes.h"
#include "sim/Simulator.h"
#include "sim/ThreadPool.h"
#include "sim/World.h"

#include <cmath>
#include <memory>

using namespace notita;

namespace {

const char* kFlameMaterials = R"(
[stone]
kind = solid
density = 2600
conductivity = 0.2
heat_capacity = 1.0
color = #808080

[timber]
kind = solid
density = 600
conductivity = 0.04
heat_capacity = 1.2
ignite_point = 300
burn_time = 120
burn_heat = 16
flame_chance = 0.22
burns_into = smoke
burn_residue = ash
residue_chance = 0.35
color = #7b5232

[ash]
kind = powder
density = 700
color = #5a5550

[smoke]
kind = gas
density = 0.9
heat_capacity = 0.4
condense_point = 45
condenses_into = ash
change_chance = 0.01
color = #4a4648

[hgas]
kind = gas
density = 0.09
heat_capacity = 0.3
ignite_point = 500
burn_time = 2
burn_heat = 80
burns_into = steam
flame_temp = 2000
color = #e6ecff

[steam]
kind = gas
density = 0.6
heat_capacity = 0.5
color = #d0d8e0

[flame]
kind = special
density = 0.3
conductivity = 0.6
heat_capacity = 0.3
spawn_temp = 950
life_min = 10
life_max = 30
flame_fuel = 30
ember = ember
color = #ff9a2a

[ember]
kind = special
density = 0.3
conductivity = 0.3
heat_capacity = 0.2
spawn_temp = 820
life_min = 8
life_max = 20
color = #ffb347
)";

struct FlameFixture {
    MaterialRegistry mats;
    std::unique_ptr<World> world;
    std::unique_ptr<ThreadPool> pool;
    std::unique_ptr<Simulator> sim;
    std::unique_ptr<GasGrid> grid;
    Rng rng{5};

    FlameFixture(int chunksW, int chunksH, int threads = 4, std::uint64_t seed = 1, bool gridFlames = true) {
        std::vector<std::string> errors;
        mats.loadFromString(kFlameMaterials, &errors);
        for (auto& e : errors) std::printf("    %s\n", e.c_str());
        CHECK(errors.empty());
        world = std::make_unique<World>(0, 0, chunksW, chunksH);
        pool = std::make_unique<ThreadPool>(threads);
        sim = std::make_unique<Simulator>(*world, mats, pool.get(), seed);
        grid = std::make_unique<GasGrid>(*world, mats, pool.get(), seed);
        sim->setGasExchange(grid.get());
        sim->params().ambientRate = 0.0f;
        sim->params().gridFlames = gridFlames;
        grid->params().gridFlames = gridFlames;
    }
    MaterialId id(const char* n) const { return mats.idOf(n); }
    void fill(const char* m, int x0, int y0, int x1, int y1, float t = kAmbientTemp) {
        scenes::fillRect(*world, id(m), x0, y0, x1, y1, rng, t);
    }
    void box(int x0, int y0, int x1, int y1) {
        fill("stone", x0 - 1, y0 - 1, x1 + 1, y0 - 1);
        fill("stone", x0 - 1, y1 + 1, x1 + 1, y1 + 1);
        fill("stone", x0 - 1, y0, x0 - 1, y1);
        fill("stone", x1 + 1, y0, x1 + 1, y1);
    }
    void tick() {
        sim->tick();
        grid->step();
    }
    std::size_t count(const char* m) const { return world->count(id(m)); }
    std::int64_t totalMass() const {
        return static_cast<std::int64_t>(world->totalMass(mats)) * kGasMassOne + grid->massFixed();
    }
    // a timber stack in a tall room, lit at its base
    void timberRoom() {
        box(4, 4, 123, 123);
        for (int k = 0; k < 4; ++k) fill("timber", 40, 120 - k * 10, 88, 117 - k * 10); // 4 planks, gaps between
        fill("timber", 40, 85, 44, 123);
        fill("timber", 84, 85, 88, 123);
        fill("timber", 50, 118, 78, 120, 450.0f); // the hot bottom plank catches first
    }
};

} // namespace

TEST_CASE(flames_burn_on_the_grid_with_exact_mass) {
    auto runWith = [](bool gridFlames, bool& exact, long& flamePx, double& burned, float& hottest) {
        FlameFixture f(2, 2, 4, 3, gridFlames);
        f.timberRoom();
        const std::size_t t0 = f.count("timber");
        const std::int64_t m0 = f.totalMass();
        exact = true;
        flamePx = 0;
        burned = 0;
        hottest = 0;
        for (int t = 0; t < 900; ++t) {
            f.tick();
            exact = exact && f.totalMass() == m0;
            flamePx += static_cast<long>(f.count("flame"));
            burned += f.grid->stats().fuelBurned;
            hottest = std::max(hottest, f.grid->stats().maxTemp);
        }
        return t0 - f.count("timber");
    };
    bool exactNew = false, exactOld = false;
    long flameNew = 0, flameOld = 0;
    double burnedNew = 0, burnedOld = 0;
    float hotNew = 0, hotOld = 0;
    const std::size_t goneNew = runWith(true, exactNew, flameNew, burnedNew, hotNew);
    const std::size_t goneOld = runWith(false, exactOld, flameOld, burnedOld, hotOld);
    std::printf("    timber burnt: grid flames %zu, pixel flames %zu; flame px-ticks %ld vs %ld; fuel burnt %.0f, hottest %.0f C\n",
                goneNew, goneOld, flameNew, flameOld, burnedNew, static_cast<double>(hotNew));
    CHECK(exactNew);
    CHECK(exactOld);
    CHECK(flameNew == 0);         // no flame pixels: the flame lives on the grid
    CHECK(flameOld > 1000);       // the old rule throws them
    CHECK(burnedNew > 1000.0);    // fuel burnt on the grid
    CHECK(burnedOld == 0.0);
    CHECK(hotNew > 1000.0f);      // the burning gas glows hot
    CHECK(goneNew >= 80);             // the lit plank burns away
    CHECK(goneNew * 10 >= goneOld * 8); // about as fast as with pixel flames
}

TEST_CASE(flames_on_the_grid_are_deterministic_across_threads) {
    auto runWith = [](int threads) {
        FlameFixture f(2, 2, threads, 9);
        f.timberRoom();
        f.fill("hgas", 10, 10, 30, 30);
        for (int t = 0; t < 400; ++t) {
            f.tick();
            if (t == 200) f.grid->explode(20, 60, 10, 1.5f);
        }
        std::uint64_t h = f.world->hash();
        for (std::int32_t m : f.grid->fields().mass) h = Rng::hash(h, static_cast<std::uint32_t>(m));
        for (float v : f.grid->fuel()) h = Rng::hash(h, static_cast<std::uint64_t>(std::llround(v * 1e4)));
        for (float v : f.grid->fields().heat) h = Rng::hash(h, static_cast<std::uint64_t>(std::llround(v * 1e3)));
        return h;
    };
    const std::uint64_t a = runWith(1);
    CHECK(a == runWith(3));
    CHECK(a == runWith(8));
}

TEST_CASE(flames_brushed_flame_pixels_become_grid_fuel) {
    FlameFixture f(1, 1);
    f.box(4, 4, 59, 59);
    f.fill("flame", 20, 40, 30, 50);
    CHECK(f.count("flame") > 100u);
    f.tick();
    CHECK(f.count("flame") == 0u);
    double burned = 0;
    for (int t = 0; t < 20; ++t) {
        f.tick();
        burned += f.grid->stats().fuelBurned;
    }
    CHECK(burned > 100.0);
    CHECK(f.grid->sourceHeat() > 100.0);
}

TEST_CASE(flames_blast_makes_a_fireball_and_hydrogen_burns_blue_hot) {
    FlameFixture f(2, 2);
    f.box(4, 4, 123, 123);
    f.fill("hgas", 70, 20, 110, 50);
    f.tick();
    const std::int64_t m0 = f.totalMass();
    f.grid->explode(40, 90, 14, 1.5f);
    int fireball = 0;
    float hottest = 0;
    bool exact = true;
    for (int t = 0; t < 12; ++t) {
        f.tick();
        fireball = std::max(fireball, f.grid->stats().flameCells);
        exact = exact && f.totalMass() == m0;
    }
    CHECK(fireball > 20);
    // light the hydrogen: it burns as a tinted, very hot flame on the grid
    f.fill("flame", 90, 52, 92, 54);
    const int hk = f.grid->speciesOf(f.id("hgas"));
    bool tinted = false;
    for (int t = 0; t < 120; ++t) {
        f.tick();
        exact = exact && f.totalMass() == m0;
        hottest = std::max(hottest, f.grid->stats().maxTemp);
        for (int c = 0; c < static_cast<int>(f.grid->fields().cells()) && !tinted; ++c)
            tinted = f.grid->flameTint(c) > 0.5f && f.grid->flameTintMat(c) == f.id("hgas");
    }
    std::printf("    fireball %d cells, hydrogen left %lld, hottest %.0f C\n", fireball, (long long)f.grid->massFixed(hk),
                static_cast<double>(hottest));
    CHECK(exact);
    CHECK(tinted);
    CHECK(hottest > 1500.0f);
}

TEST_CASE(flames_fuel_survives_the_chunk_codec_and_sleep) {
    FlameFixture f(2, 2);
    f.timberRoom();
    for (int t = 0; t < 120; ++t) f.tick();
    CHECK(f.grid->fuelEnergy() > 1.0);
    // the chunk with the most fuel in it
    int bx = 0, by = 0;
    double fa = -1;
    GasGrid::ChunkCells a;
    for (int cy = 0; cy < 2; ++cy)
        for (int cx = 0; cx < 2; ++cx) {
            GasGrid::ChunkCells t;
            CHECK(f.grid->exportChunk(cx, cy, t, false));
            double s = 0;
            for (float v : t.fuel) s += v;
            if (s > fa) { fa = s; bx = cx; by = cy; a = t; }
        }
    GasGrid::ChunkCells b;
    CHECK(decodeGas(encodeGas(a), b, f.grid->speciesCount(), nullptr));
    double fb = 0;
    for (float v : b.fuel) fb += v;
    CHECK(fa > 0.0);
    CHECK(fa == fb);
    CHECK(b.massFixed() == a.massFixed());
    // put to sleep (cleared) and woken again: the fuel comes back
    const double before = f.grid->fuelEnergy();
    GasGrid::ChunkCells c;
    CHECK(f.grid->exportChunk(bx, by, c, true));
    CHECK(f.grid->fuelEnergy() < before);
    CHECK(f.grid->importChunk(bx, by, c));
    CHECK(std::abs(f.grid->fuelEnergy() - before) < 1e-3 * before + 1e-3);
}
