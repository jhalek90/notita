// Phase 3 tests: the gas grid. The solver math runs on CpuGasSolver (the
// reference the GPU kernels mirror), so none of this needs a GPU.
#include "test.h"

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

// Own material table so retuning data/ doesn't break these.
const char* kGasMaterials = R"(
[stone]
kind = solid
density = 2600
conductivity = 0.2
heat_capacity = 1.0
color = #808080

[sand]
kind = powder
density = 1600
color = #d0b060

[water]
kind = liquid
density = 1000
spread = 6
conductivity = 0.3
heat_capacity = 1.0
boil_point = 100
boils_into = steam
latent_heat = 8
color = #3070c0

[steam]
kind = gas
density = 0.6
conductivity = 0.05
heat_capacity = 0.5
condense_point = 70
condenses_into = water
color = #d0d8e0

[fizz]
kind = gas
density = 0.1
heat_capacity = 0.4
color = #e0e8ff

[heavy]
kind = gas
density = 3.5
heat_capacity = 0.4
color = #507030

[hgas]
kind = gas
density = 0.09
heat_capacity = 0.3
ignite_point = 500
burn_time = 2
burn_heat = 80
burns_into = steam
color = #e6ecff

[flame]
kind = special
density = 0.3
conductivity = 0.6
heat_capacity = 0.3
spawn_temp = 950
life_min = 10
life_max = 20
color = #ff9a2a
)";

struct GasFixture {
    MaterialRegistry mats;
    std::unique_ptr<World> world;
    std::unique_ptr<ThreadPool> pool;
    std::unique_ptr<Simulator> sim;
    std::unique_ptr<GasGrid> grid;
    Rng rng{5};

    GasFixture(int chunksW, int chunksH, int threads = 4, std::uint64_t seed = 1) {
        std::vector<std::string> errors;
        mats.loadFromString(kGasMaterials, &errors);
        for (auto& e : errors) std::printf("    %s\n", e.c_str());
        CHECK(errors.empty());
        world = std::make_unique<World>(0, 0, chunksW, chunksH);
        pool = std::make_unique<ThreadPool>(threads);
        sim = std::make_unique<Simulator>(*world, mats, pool.get(), seed);
        grid = std::make_unique<GasGrid>(*world, mats, pool.get(), seed);
        sim->setGasExchange(grid.get());
        sim->params().ambientRate = 0.0f; // the grid carries the air's heat
    }
    MaterialId id(const char* n) const { return mats.idOf(n); }
    void fill(const char* m, int x0, int y0, int x1, int y1, float t = kAmbientTemp) {
        scenes::fillRect(*world, id(m), x0, y0, x1, y1, rng, t);
    }
    void box(int x0, int y0, int x1, int y1, float t = kAmbientTemp) {
        fill("stone", x0 - 1, y0 - 1, x1 + 1, y0 - 1, t);
        fill("stone", x0 - 1, y1 + 1, x1 + 1, y1 + 1, t);
        fill("stone", x0 - 1, y0, x0 - 1, y1, t);
        fill("stone", x1 + 1, y0, x1 + 1, y1, t);
    }
    void tick() {
        sim->tick();
        grid->step();
    }
    void run(int n) { for (int i = 0; i < n; ++i) tick(); }
    std::size_t count(const char* m) const { return world->count(id(m)); }
    // world mass and grid mass in the same fixed-point units
    std::int64_t totalMass() const {
        return static_cast<std::int64_t>(world->totalMass(mats)) * kGasMassOne + grid->massFixed();
    }
    // heat above ambient in pixels and grid, plus what left through the ambient
    double totalHeat() const {
        double e = 0;
        for (const Chunk* c : world->chunks())
            for (const Cell& cell : c->cells) e += static_cast<double>(mats[cell.material].heatCapacity) * (cell.temperature - kAmbientTemp);
        return e + grid->heatEnergy() + grid->lostHeat();
    }
    // mass-weighted mean grid row of one species (0 = top)
    double meanRow(const char* gas) const {
        const int k = grid->speciesOf(id(gas));
        const GasFields& f = grid->fields();
        double s = 0, m = 0;
        for (int j = 0; j < f.h; ++j)
            for (int i = 0; i < f.w; ++i) {
                const double v = f.m(k, j * f.w + i);
                s += v * j;
                m += v;
            }
        return m > 0 ? s / m : -1.0;
    }
};

double maxDivergence(const GasFields& f) {
    double worst = 0;
    for (int j = 0; j < f.h; ++j)
        for (int i = 0; i < f.w; ++i) {
            if (f.open[j * f.w + i] <= 0.0f) continue;
            const double d = f.u[j * (f.w + 1) + i + 1] - f.u[j * (f.w + 1) + i] + f.v[(j + 1) * f.w + i] - f.v[j * f.w + i];
            worst = std::max(worst, std::abs(d));
        }
    return worst;
}

} // namespace

TEST_CASE(gas_solver_projection_removes_divergence) {
    GasFields f;
    f.resize(32, 24, 1);
    std::fill(f.open.begin(), f.open.end(), 1.0f);
    for (int i = 10; i < 14; ++i)
        for (int j = 8; j < 16; ++j) f.open[j * f.w + i] = 0.0f; // a pillar in the middle
    Rng rng(3);
    for (float& u : f.u) u = rng.unit() - 0.5f;
    for (float& v : f.v) v = rng.unit() - 0.5f;
    GasSolveParams sp;
    sp.w = f.w;
    sp.h = f.h;
    sp.species = 1;
    sp.buoyancyHeat = sp.buoyancyDensity = sp.vorticity = 0.0f;
    sp.damping = 1.0f;
    sp.pressureIters = 600;
    sp.cap[0] = 0.5f / kGasMassOne;
    CpuGasSolver solver;
    solver.solve(f, sp);
    CHECK(maxDivergence(f) < 0.02);
    // nothing flows through walls or into the pillar
    bool walls = true;
    for (int j = 0; j < f.h; ++j) walls = walls && f.u[j * (f.w + 1)] == 0.0f && f.u[j * (f.w + 1) + f.w] == 0.0f;
    for (int j = 8; j < 16; ++j) walls = walls && f.u[j * (f.w + 1) + 10] == 0.0f && f.u[j * (f.w + 1) + 14] == 0.0f;
    CHECK(walls);
}

TEST_CASE(gas_advection_keeps_mass_exactly) {
    GasFields f;
    f.resize(40, 30, 2);
    std::fill(f.open.begin(), f.open.end(), 1.0f);
    Rng rng(9);
    std::int64_t total = 0;
    for (int c = 0; c < static_cast<int>(f.cells()); ++c)
        for (int k = 0; k < 2; ++k) {
            const auto m = static_cast<std::int32_t>(rng.below(5 * kGasMassOne));
            f.m(k, c) = m;
            total += m;
        }
    for (float& u : f.u) u = 3.0f * (rng.unit() - 0.5f);
    for (float& v : f.v) v = 3.0f * (rng.unit() - 0.5f);
    GasSolveParams sp;
    sp.w = f.w;
    sp.h = f.h;
    sp.species = 2;
    sp.light[0] = 0.9f;
    sp.light[1] = -2.0f;
    sp.cap[0] = sp.cap[1] = 0.4f / kGasMassOne;
    CpuGasSolver solver;
    bool exact = true, positive = true;
    for (int t = 0; t < 200; ++t) {
        solver.solve(f, sp);
        std::int64_t s = 0;
        for (std::int32_t m : f.mass) { s += m; positive = positive && m >= 0; }
        exact = exact && s == total;
    }
    CHECK(exact);
    CHECK(positive);
}

TEST_CASE(gas_grid_boils_and_condenses_with_exact_mass) {
    GasFixture f(1, 2);
    f.grid->params().ambientLoss = 0.01f; // the steam cools off and rains back
    f.box(4, 4, 59, 123);
    f.fill("water", 4, 100, 59, 123, 200.0f); // boils at once
    const std::size_t water0 = f.count("water");
    const std::int64_t mass0 = f.totalMass();
    bool exact = true;
    std::int64_t peak = 0;
    std::size_t minWater = water0;
    for (int t = 0; t < 2500; ++t) {
        f.tick();
        exact = exact && f.totalMass() == mass0;
        peak = std::max(peak, f.grid->massFixed());
        minWater = std::min(minWater, f.count("water"));
    }
    CHECK(exact);
    CHECK(peak > 300 * kGasMassOne);             // a good part went into the grid as steam
    CHECK(minWater < water0 / 2);
    CHECK(f.count("water") > minWater + 200);    // and came back as water
    CHECK(f.count("steam") == 0u);               // never as stray steam pixels
    CHECK(f.grid->stats().condensed >= 0);
}

std::size_t countOf(const GasFixture& f, const char* m) { return f.world->count(f.id(m)); }

TEST_CASE(gas_grid_keeps_heat_in_a_sealed_box) {
    GasFixture f(1, 1);
    f.grid->params().ambientLoss = 0.0f;
    f.box(8, 8, 55, 55);
    f.fill("fizz", 20, 40, 40, 54, 300.0f); // hot light gas, taken in on the first step
    f.fill("stone", 8, 30, 14, 34, 150.0f);  // and a warm ledge that heats the air
    const double e0 = f.totalHeat();
    f.run(600);
    const double e1 = f.totalHeat();
    CHECK(countOf(f, "fizz") == 0u);
    CHECK(std::abs(e1 - e0) / std::abs(e0) < 2e-3);
    // heat moved out of the gas into the walls, so it was carried around
    CHECK(f.grid->heatEnergy() < 0.9 * (e0 - 6.0 * 5.0 * 130.0));
}

TEST_CASE(gas_grid_light_gas_rises_heavy_gas_pools) {
    GasFixture f(1, 2);
    f.box(4, 4, 59, 123);
    f.fill("fizz", 20, 100, 40, 120);
    f.fill("heavy", 20, 10, 40, 30);
    f.tick();
    const double fizz0 = f.meanRow("fizz"), heavy0 = f.meanRow("heavy");
    f.run(900);
    CHECK(f.meanRow("fizz") < fizz0 - 8.0);   // rose by more than 32 px
    CHECK(f.meanRow("heavy") > heavy0 + 8.0); // sank
    // nothing leaked into the walls or out of the box
    const GasFields& g = f.grid->fields();
    bool sealed = true;
    for (int j = 0; j < g.h; ++j)
        for (int i = 0; i < g.w; ++i)
            if (g.open[j * g.w + i] <= 0.0f)
                for (int k = 0; k < g.species; ++k) sealed = sealed && g.m(k, j * g.w + i) == 0;
    CHECK(sealed);
}

TEST_CASE(gas_grid_hot_air_rises) {
    // a hot plate on the floor of a tall box; with the grid the heat reaches the
    // ceiling by convection, without it only by (slow) conduction through air
    auto ceiling = [](bool grid, float& speed) {
        GasFixture f(1, 2);
        f.grid->params().enabled = grid;
        f.grid->params().ambientLoss = 0.0f;
        f.box(4, 4, 59, 123);
        f.fill("stone", 24, 116, 40, 123, 600.0f);
        f.run(400);
        double top = 0;
        for (int x = 20; x < 44; ++x) top += f.world->cell(x, 8)->temperature;
        speed = f.grid->stats().maxSpeed;
        return top / 24.0;
    };
    float speed = 0, still = 0;
    const double withGrid = ceiling(true, speed), without = ceiling(false, still);
    std::printf("    ceiling %.1f C with the grid, %.1f C without; max speed %.3f\n", withGrid, without, speed);
    CHECK(withGrid > kAmbientTemp + 5.0);
    CHECK(withGrid > without + 4.0);
    CHECK(speed > 0.05f);
}

TEST_CASE(gas_grid_explosion_throws_sand_and_keeps_mass) {
    GasFixture f(2, 2);
    f.fill("stone", 0, 120, 127, 127);
    f.fill("sand", 50, 90, 78, 119);
    f.fill("water", 80, 100, 110, 119);
    f.run(5);
    const std::int64_t mass0 = f.totalMass();
    auto sandNear = [&] {
        int n = 0;
        for (int y = 100; y <= 119; ++y)
            for (int x = 54; x <= 74; ++x) n += f.world->material(x, y) == f.id("sand");
        return n;
    };
    const int before = sandNear();
    f.grid->explode(64, 112, 14, 2.0f);
    std::printf("    sand near %d -> %d\n", before, sandNear());
    CHECK(sandNear() < before - 50);
    double sumSpeed = 0;
    f.tick();
    sumSpeed = f.grid->stats().maxSpeed;
    CHECK(sumSpeed > 0.2);
    bool exact = f.totalMass() == mass0;
    for (int t = 0; t < 120; ++t) {
        f.tick();
        exact = exact && f.totalMass() == mass0;
    }
    CHECK(exact);
    CHECK(f.grid->sourceHeat() > 0.0);
}

TEST_CASE(gas_grid_flammable_gas_burns_into_steam) {
    GasFixture f(1, 1);
    f.box(4, 4, 59, 59);
    f.fill("hgas", 10, 30, 50, 58);
    f.tick();
    const int h = f.grid->speciesOf(f.id("hgas")), s = f.grid->speciesOf(f.id("steam"));
    const std::int64_t h0 = f.grid->massFixed(h);
    const std::int64_t mass0 = f.totalMass();
    CHECK(h0 > 0);
    f.fill("flame", 30, 40, 31, 41);
    bool exact = true;
    for (int t = 0; t < 200; ++t) {
        f.tick();
        exact = exact && f.totalMass() == mass0;
    }
    CHECK(exact);
    std::printf("    hgas %lld -> %lld, steam %lld, water %zu\n", (long long)h0, (long long)f.grid->massFixed(h),
                (long long)f.grid->massFixed(s), f.count("water"));
    CHECK(f.grid->massFixed(h) < h0 / 10); // went up in a chain
    CHECK(f.grid->massFixed(s) + static_cast<std::int64_t>(f.count("water")) * kGasMassOne > h0 / 2);
    CHECK(f.grid->sourceHeat() > 0.0);
}

TEST_CASE(gas_grid_determinism_independent_of_thread_count) {
    auto runWith = [](int threads) {
        GasFixture f(2, 2, threads, 42);
        f.grid->params().ambientLoss = 0.01f;
        f.box(4, 4, 123, 123);
        f.fill("water", 10, 100, 60, 123, 180.0f);
        f.fill("heavy", 70, 10, 110, 30);
        f.fill("sand", 80, 90, 100, 110);
        for (int t = 0; t < 300; ++t) {
            f.tick();
            if (t == 100) f.grid->explode(90, 105, 10, 1.5f);
        }
        std::uint64_t h = f.world->hash();
        for (std::int32_t m : f.grid->fields().mass) h = Rng::hash(h, static_cast<std::uint32_t>(m));
        for (float v : f.grid->fields().u) h = Rng::hash(h, static_cast<std::uint64_t>(std::llround(v * 1e6)));
        return h;
    };
    const std::uint64_t a = runWith(1);
    CHECK(a == runWith(3));
    CHECK(a == runWith(8));
}
