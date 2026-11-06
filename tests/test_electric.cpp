// Electricity tests: the potential solve (Ohm's law), resistive heating and
// melting, lamps, switches, pressure plates, sparks, electrolysis with mass
// kept, lightning shocking a swimmer, determinism, and potentials carried
// through streaming sleep/wake and save/load.
#include "test.h"

#include "sim/Electric.h"
#include "sim/GasGrid.h"
#include "sim/Material.h"
#include "sim/Particles.h"
#include "sim/Player.h"
#include "sim/Scenes.h"
#include "sim/Simulator.h"
#include "sim/Streamer.h"
#include "sim/ThreadPool.h"
#include "sim/World.h"
#include "sim/WorldGen.h"

#include <cmath>
#include <cstdio>
#include <memory>

using namespace notita;

namespace {

// Own table (names the world generator also looks for, plus the electric parts).
const char* kElecMaterials = R"(
[bedrock]
kind = solid
density = 5000
anchor = true
[slate]
kind = solid
density = 2700
conductivity = 0.2
[basalt]
kind = solid
density = 2900
[loam]
kind = solid
density = 1600
[turf]
kind = solid
density = 1300
[sand]
kind = powder
density = 1600
[cinder_grit]
kind = powder
density = 1900
[magma]
kind = liquid
density = 2800
viscosity = 0.92
spawn_temp = 1300
[lamp_oil]
kind = liquid
density = 820
[murk]
kind = gas
density = 3.0
[steam]
kind = gas
density = 0.6
heat_capacity = 0.5
[water]
kind = liquid
density = 1000
spread = 6
conductivity = 0.3
electric = 0.02
splits_into = hydrogen, oxygen
split_current = 0.02
[brine]
kind = liquid
density = 1100
spread = 6
conductivity = 0.3
electric = 0.3
splits_into = hydrogen, oxygen
[hydrogen]
kind = gas
density = 0.09
[oxygen]
kind = gas
density = 1.43
[iron]
kind = solid
density = 7800
conductivity = 0.6
electric = 0.6
[copper]
kind = solid
density = 8900
conductivity = 0.9
heat_capacity = 0.4
electric = 1.0
melt_point = 1085
melts_into = molten_copper
change_chance = 1
[molten_copper]
kind = liquid
density = 8000
spread = 3
conductivity = 0.8
heat_capacity = 0.45
spawn_temp = 1150
freeze_point = 1040
freezes_into = copper
electric = 0.7
[heater_coil]
kind = solid
density = 8400
conductivity = 0.5
heat_capacity = 0.6
electric = 0.05
[volt_cell]
kind = solid
density = 3000
electric_role = source
voltage = 100
[ground_rod]
kind = solid
density = 7000
electric_role = drain
[thermopile]
kind = solid
density = 6000
conductivity = 0.6
electric_role = thermopile
voltage = 0.1
[switch_open]
kind = solid
density = 3000
electric_role = switch
toggles_into = switch_closed
[switch_closed]
kind = solid
density = 3000
electric = 1.0
electric_role = switch
toggles_into = switch_open
[press_plate]
kind = solid
density = 4000
electric = 1.0
electric_role = plate
[lamp]
kind = solid
density = 2500
electric = 0.2
electric_role = lamp
toggles_into = lamp_lit
[lamp_lit]
kind = solid
density = 2500
electric = 0.2
electric_role = lamp_lit
toggles_into = lamp
[timber]
kind = solid
density = 600
conductivity = 0.04
ignite_point = 300
burn_time = 200
burn_heat = 16
burns_into = steam
[spark]
kind = special
density = 0.2
conductivity = 0.6
heat_capacity = 0.2
spawn_temp = 1600
life_min = 2
life_max = 7
)";

MaterialRegistry loadElecMats() {
    MaterialRegistry mats;
    std::vector<std::string> errors;
    mats.loadFromString(kElecMaterials, &errors);
    for (auto& e : errors) std::printf("  materials: %s\n", e.c_str());
    CHECK(errors.empty());
    return mats;
}

// A small fixed world (no gas grid, no bodies): sim + electricity (+ player).
struct Bench {
    MaterialRegistry mats = loadElecMats();
    World world{0, 0, 4, 3};
    std::unique_ptr<ThreadPool> pool;
    std::unique_ptr<Simulator> sim;
    std::unique_ptr<ElectricSystem> elec;
    std::unique_ptr<Player> player;
    PlayerInput in;
    bool withPlayer = false;
    explicit Bench(int threads = 4, bool heat = true) {
        pool = std::make_unique<ThreadPool>(threads);
        sim = std::make_unique<Simulator>(world, mats, pool.get(), 7);
        sim->params().heat = heat;
        sim->params().reactions = false;
        sim->params().ambientRate = 0.0f;
        elec = std::make_unique<ElectricSystem>(world, mats, pool.get(), 7);
        for (auto& e : elec->errors()) std::printf("  electric: %s\n", e.c_str());
        CHECK(elec->errors().empty());
        player = std::make_unique<Player>(world, mats, 7);
        player->setElectric(elec.get());
        player->inventory().reset(mats.size());
    }
    MaterialId id(const char* n) const { return mats.idOf(n); }
    void put(int x, int y, const char* m) { world.place(x, y, id(m), 128, mats[id(m)].spawnTemp); }
    void line(int x0, int x1, int y, const char* m) { for (int x = x0; x <= x1; ++x) put(x, y, m); }
    void tick() {
        sim->tick();
        elec->step();
        if (withPlayer) player->step(in);
    }
    void ticks(int n) { for (int i = 0; i < n; ++i) tick(); }
};

} // namespace

TEST_CASE(electric_wire_obeys_ohms_law) {
    Bench b(4, false);
    // source, 49 px of copper, drain: 50 unit edges -> 2 units of current, a linear potential
    const int y = 40;
    b.put(10, y, "volt_cell");
    b.line(11, 59, y, "copper");
    b.put(60, y, "ground_rod");
    b.ticks(2);
    const ElectricStats& st = b.elec->stats();
    CHECK_EQ(st.components, 1);
    CHECK_EQ(st.nodes, 51);
    CHECK(st.residual < 0.05);
    CHECK(std::abs(b.elec->currentAt(35, y) - 2.0f) < 0.01f);
    CHECK(std::abs(b.elec->potentialAt(35, y) - 50.0f) < 0.5f);
    CHECK(std::abs(st.power - 200.0) < 2.0); // V * I
    // nothing connected: no field, no current
    CHECK(b.elec->currentAt(35, y + 5) == 0.0f);
    // a lone source (no drain) charges its wire but carries no current
    Bench c(1, false);
    c.put(10, y, "volt_cell");
    c.line(11, 30, y, "copper");
    c.ticks(2);
    CHECK(std::abs(c.elec->potentialAt(25, y) - 100.0f) < 1e-3f);
    CHECK(c.elec->currentAt(25, y) == 0.0f);
}

TEST_CASE(electric_short_circuit_heats_and_melts_the_wire) {
    Bench b;
    const int y = 60;
    b.put(20, y, "volt_cell");
    b.line(21, 32, y, "copper");
    b.put(33, y, "ground_rod");
    // a heater in series elsewhere: the coil gets most of the power
    const int y2 = 100;
    b.put(20, y2, "volt_cell");
    b.line(21, 60, y2, "copper");
    b.line(61, 70, y2, "heater_coil");
    b.line(71, 110, y2, "copper");
    b.put(111, y2, "ground_rod");
    scenes::pinRect(b.world, b.mats, 0, 0, 255, 191);
    b.ticks(1);
    const float wireCur = b.elec->currentAt(26, y);
    std::printf("  short: %.2f units, heater circuit: %.3f units\n", wireCur, b.elec->currentAt(40, y2));
    float coilHot = 0, wireCool = 0;
    bool melted = false;
    for (int i = 0; i < 200 && !melted; ++i) {
        b.tick();
        for (int x = 21; x <= 32; ++x) melted |= b.world.material(x, y) == b.id("molten_copper");
    }
    CHECK(melted);
    b.ticks(200);
    coilHot = b.world.cell(65, y2)->temperature;
    wireCool = b.world.cell(40, y2)->temperature;
    std::printf("  heater coil %.0f C, its copper lead %.0f C\n", coilHot, wireCool);
    CHECK(coilHot > 100.0f);
    CHECK(coilHot > wireCool + 50.0f);
}

TEST_CASE(electric_switch_lamp_and_pressure_plate) {
    Bench b(4, false);
    const int y = 50;
    b.put(10, y, "volt_cell");
    b.line(11, 20, y, "copper");
    b.put(21, y, "switch_open");
    b.line(22, 30, y, "copper");
    b.line(31, 33, y, "lamp");
    b.line(34, 40, y, "copper");
    b.put(41, y, "press_plate");
    b.put(41, y + 1, "press_plate"); // a thick plate: its own top row isn't a load
    b.line(42, 50, y, "copper");
    b.put(51, y, "ground_rod");
    b.ticks(2);
    CHECK_EQ(b.world.material(32, y), b.id("lamp"));
    CHECK(b.elec->toggleSwitch(22, y + 2, 4) == 1); // nearest switch pixel, connected ones flip together
    CHECK_EQ(b.world.material(21, y), b.id("switch_closed"));
    b.ticks(2);
    CHECK_EQ(b.world.material(32, y), b.id("lamp")); // plate still up
    b.put(41, y - 1, "slate");                       // weight on the plate
    b.ticks(2);
    CHECK_EQ(b.world.material(32, y), b.id("lamp_lit"));
    CHECK_EQ(b.elec->stats().lit, 3);
    b.world.place(41, y - 1, kEmpty, 128);
    b.ticks(2);
    CHECK_EQ(b.world.material(32, y), b.id("lamp"));
    // the player standing on the plate presses it too
    b.player->spawn(41.5f, static_cast<float>(y));
    b.elec->setPlayer(b.player.get());
    b.ticks(2);
    CHECK_EQ(b.world.material(32, y), b.id("lamp_lit"));
    b.elec->toggleSwitch(21, y, 2);
    b.ticks(2);
    CHECK_EQ(b.world.material(32, y), b.id("lamp"));
}

TEST_CASE(electric_sparks_fly_and_light_timber) {
    Bench b;
    const int y = 80;
    b.put(20, y, "volt_cell");
    b.line(21, 28, y, "iron"); // doesn't melt in this table, so it keeps sparking
    b.put(29, y, "ground_rod");
    // timber just above the shorted wire (a gap row for the sparks)
    b.line(18, 31, y - 2, "timber");
    scenes::pinRect(b.world, b.mats, 0, 0, 255, 191);
    int sparks = 0;
    bool burning = false;
    for (int i = 0; i < 120; ++i) {
        b.tick();
        sparks += b.elec->stats().sparks;
        for (int x = 18; x <= 31; ++x)
            if (const Cell* c = b.world.cell(x, y - 2)) burning |= (c->flags & kFlagBurning) != 0;
    }
    std::printf("  %d sparks\n", sparks);
    CHECK(sparks > 0);
    CHECK(burning);
}

TEST_CASE(electric_electrolysis_keeps_mass) {
    Bench b(4, false);
    // a basin of water with an electrode at each end
    const int x0 = 20, x1 = 80, yb = 120;
    for (int y = yb - 20; y <= yb; ++y) { b.put(x0 - 1, y, "slate"); b.put(x1 + 1, y, "slate"); }
    b.line(x0 - 1, x1 + 1, yb + 1, "slate");
    for (int y = yb - 14; y <= yb; ++y) {
        b.put(x0, y, y < yb - 10 ? "slate" : "volt_cell");
        b.put(x1, y, y < yb - 10 ? "slate" : "ground_rod");
        for (int x = x0 + 1; x < x1; ++x) b.put(x, y, "water");
    }
    scenes::pinRect(b.world, b.mats, 0, 0, 255, 191);
    b.elec->params().splitRate = 0.003f; // fast, so the test is short
    const long long m0 = b.world.totalMass(b.mats);
    b.ticks(300);
    const long long h = static_cast<long long>(b.world.count(b.id("hydrogen")));
    const long long o = static_cast<long long>(b.world.count(b.id("oxygen")));
    std::printf("  %lld splits: %lld hydrogen, %lld oxygen pixels\n", b.elec->stats().splitsTotal, h, o);
    CHECK(b.elec->stats().splitsTotal > 0);
    CHECK_EQ(h, b.elec->stats().splitsTotal);
    CHECK_EQ(o, b.elec->stats().splitsTotal);
    CHECK_EQ(b.world.totalMass(b.mats), m0);
}

TEST_CASE(electric_lightning_shocks_a_swimmer) {
    Bench b(4, true);
    const int x0 = 60, x1 = 160, yb = 150;
    b.line(x0 - 1, x1 + 1, yb + 1, "slate");
    for (int y = yb - 30; y <= yb; ++y) {
        b.put(x0 - 1, y, "slate");
        b.put(x1 + 1, y, "slate");
        for (int x = x0; x <= x1; ++x) b.put(x, y, "water");
    }
    scenes::pinRect(b.world, b.mats, 0, 0, 255, 191);
    b.withPlayer = true;
    b.player->spawn(115.0f, static_cast<float>(yb - 4));
    b.ticks(5);
    const float hp0 = b.player->health();
    CHECK(b.player->stats().shock == 0.0f);
    b.elec->strike(112, 10);
    b.tick();
    std::printf("  shock %.2f, damage %.2f, water at the hit %.0f C\n", b.player->stats().shock, b.player->stats().damage,
                b.world.cell(112, yb - 30) ? b.world.cell(112, yb - 30)->temperature : 0.0f);
    CHECK(b.elec->stats().strikes == 1);
    CHECK(b.player->stats().shock > 0.0f);
    CHECK(b.player->health() < hp0);
    b.ticks(3);
    CHECK(b.player->stats().shock == 0.0f); // a strike is one tick of current
}

TEST_CASE(electric_thermopile_drives_current_when_hot) {
    Bench b(1, false);
    const int y = 30;
    b.put(10, y, "thermopile");
    b.line(11, 29, y, "copper");
    b.put(30, y, "ground_rod");
    b.ticks(2);
    CHECK(b.elec->currentAt(20, y) == 0.0f);
    b.world.cell(10, y)->temperature = 520.0f; // 0.1 V per degree above ambient -> 50 V
    b.ticks(2);
    CHECK(std::abs(b.elec->currentAt(20, y) - 2.5f) < 0.02f);
}

TEST_CASE(electric_determinism_independent_of_thread_count) {
    auto run = [](int threads) {
        Bench b(threads);
        for (int k = 0; k < 4; ++k) {
            const int y = 20 + k * 40;
            b.put(10, y, "volt_cell");
            b.line(11, 40 + k * 10, y, "copper");
            b.line(41 + k * 10, 45 + k * 10, y, "heater_coil");
            b.line(46 + k * 10, 120, y, "copper");
            b.put(121, y, "ground_rod");
            for (int x = 130; x < 200; ++x) b.put(x, y - 1, "water");
            b.put(129, y - 1, "volt_cell");
            b.put(200, y - 1, "ground_rod");
        }
        b.elec->params().maxIterations = 3; // the warm start carries over between ticks
        b.ticks(150);
        return b.world.hash() ^ static_cast<std::uint64_t>(b.elec->stats().splitsTotal);
    };
    CHECK(run(1) == run(6));
}

TEST_CASE(electric_big_circuit_is_independent_of_thread_count) {
    // a brine slab big enough for the block-parallel solver path
    auto run = [](int threads, float& probe) {
        Bench b(threads, false);
        for (int y = 40; y < 130; ++y) {
            b.put(20, y, "volt_cell");
            b.put(231, y, "ground_rod");
            for (int x = 21; x <= 230; ++x) b.put(x, y, "brine");
        }
        b.line(20, 231, 130, "copper"); // a floor that also shorts the bottom row
        b.line(19, 232, 131, "iron");
        b.elec->params().maxIterations = 20;
        b.elec->params().splitRate = 0.0005f;
        b.ticks(40);
        CHECK(b.elec->stats().nodes > 18000);
        probe = b.elec->potentialAt(120, 130); // the copper floor
        std::printf("  %d threads: %d nodes %d comps %d unsettled %d it, resid %.3f, mat %d\n", threads, b.elec->stats().nodes, b.elec->stats().components,
                    b.elec->stats().unsettled, b.elec->stats().iterations, b.elec->stats().residual, b.world.material(120, 130));
        return b.world.hash() ^ static_cast<std::uint64_t>(b.elec->stats().splitsTotal);
    };
    float p1 = 0, p6 = 0;
    const std::uint64_t h1 = run(1, p1), h6 = run(6, p6);
    CHECK(h1 == h6);
    CHECK(p1 == p6);
    std::printf("  probe %.3f\n", p1);
    CHECK(p1 > 0.0f && p1 < 100.0f);
}

TEST_CASE(electric_potentials_survive_sleep_wake_and_save_load) {
    // a streamed world (no grid, no bodies): a circuit in the sky above the terrain
    struct Rig {
        MaterialRegistry mats = loadElecMats();
        StreamParams sp;
        std::unique_ptr<WorldGen> gen;
        std::unique_ptr<World> world;
        std::unique_ptr<ThreadPool> pool;
        std::unique_ptr<Simulator> sim;
        std::unique_ptr<ElectricSystem> elec;
        std::unique_ptr<Streamer> stream;
        Rig() {
            sp.chunksW = 8;
            sp.chunksH = 5;
            gen = std::make_unique<WorldGen>(mats, 3);
            world = std::make_unique<World>(0, 0, sp.chunksW, sp.chunksH);
            pool = std::make_unique<ThreadPool>(4);
            sim = std::make_unique<Simulator>(*world, mats, pool.get(), 3);
            sim->params().reactions = false;
            elec = std::make_unique<ElectricSystem>(*world, mats, pool.get(), 3);
            elec->params().maxIterations = 2; // slow convergence: the warm start is real state
            stream = std::make_unique<Streamer>(*world, mats, *gen, sp);
            stream->attach(sim.get(), nullptr, nullptr, nullptr);
            stream->setElectric(elec.get());
            stream->setPool(pool.get());
        }
        void tick() { sim->tick(); elec->step(); }
    };
    Rig a;
    a.stream->start(0, a.gen->surfaceY(0));
    const int x0 = a.stream->windowX() * kChunkSize + 40;
    int top = a.gen->surfaceY(x0);
    for (int x = x0; x < x0 + 200; ++x) top = std::min(top, a.gen->surfaceY(x));
    const int y = std::max(a.world->minY() + 4, top - 60);
    auto put = [&](int x, const char* m) { a.world->place(x, y, a.mats.idOf(m), 128); };
    put(x0, "volt_cell");
    for (int x = x0 + 1; x < x0 + 199; ++x) put(x, "copper");
    put(x0 + 199, "ground_rod");
    scenes::pinRect(*a.world, a.mats, x0, y, x0 + 199, y);
    for (int i = 0; i < 20; ++i) a.tick();
    const float v = a.elec->potentialAt(x0 + 12, y);
    CHECK(v > 0.0f);
    CHECK(a.elec->stats().residual > 0.01); // still converging
    // sleep the circuit's chunks and wake them again: the potentials come back bit for bit
    const int wx = a.stream->windowX(), wy = a.stream->windowY();
    a.stream->shiftTo(wx + 20, wy);
    CHECK(a.elec->potentialAt(x0 + 12, y) == 0.0f);
    a.stream->shiftTo(wx, wy);
    CHECK(a.elec->potentialAt(x0 + 12, y) == v);
    // save, load into a second world, and run both in lockstep
    std::vector<std::uint8_t> bytes;
    CHECK(a.stream->saveToBytes(bytes, SaveExtras{}));
    Rig b;
    SaveExtras ex;
    std::string err;
    CHECK(b.stream->loadFromBytes(bytes, ex, &err));
    CHECK(err.empty());
    CHECK_EQ(b.elec->tickCount(), a.elec->tickCount());
    CHECK(b.elec->potentialAt(x0 + 12, y) == v);
    int diverged = -1;
    for (int i = 0; i < 40 && diverged < 0; ++i) {
        a.tick();
        b.tick();
        if (a.world->hash() != b.world->hash() || a.elec->potentialAt(x0 + 12, y) != b.elec->potentialAt(x0 + 12, y))
            diverged = i;
    }
    CHECK_EQ(diverged, -1);
}

TEST_CASE(electric_game_data_is_valid) {
    // the real data/materials.txt, when the tests run from the repo or the build dir
    MaterialRegistry mats;
    std::vector<std::string> errors;
    if (!mats.loadFromFile("data/materials.txt", &errors) && !mats.loadFromFile("../data/materials.txt", &errors)) {
        std::printf("  data/materials.txt not found, skipped\n");
        return;
    }
    for (auto& e : errors) std::printf("  materials: %s\n", e.c_str());
    CHECK(errors.empty());
    World w(0, 0, 1, 1);
    ElectricSystem e(w, mats, nullptr, 1);
    for (auto& s : e.errors()) std::printf("  electric: %s\n", s.c_str());
    CHECK(e.errors().empty());
    CHECK(e.props(mats.idOf("copper")).conduct > 0.5f);
    CHECK(e.props(mats.idOf("water")).splitA == mats.idOf("hydrogen"));
    CHECK(e.props(mats.idOf("volt_cell")).role == ElectricRole::Source);
}
