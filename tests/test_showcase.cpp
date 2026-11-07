// The showcase building (--scene showcase): a WorldOverlay laid over the
// generated world. It must stay a pure function of (x, y), stream and save
// like the terrain, light its rooms with real circuits, hold its loose blocks
// until something cuts them, and be walkable (stairs to the gallery).
#include "test.h"

#include "sim/Bodies.h"
#include "sim/Electric.h"
#include "sim/GasGrid.h"
#include "sim/Material.h"
#include "sim/Particles.h"
#include "sim/Player.h"
#include "sim/Showcase.h"
#include "sim/Simulator.h"
#include "sim/Streamer.h"
#include "sim/ThreadPool.h"
#include "sim/World.h"
#include "sim/WorldGen.h"

#include <cmath>
#include <cstring>
#include <memory>

using namespace notita;

namespace {

// The showcase is built from the game's materials (concrete, copper, lamps...).
bool loadGameMaterials(MaterialRegistry& mats) {
    std::vector<std::string> errors;
    if (!mats.loadFromFile("data/materials.txt", &errors) && (errors.clear(), !mats.loadFromFile("../data/materials.txt", &errors))) {
        std::printf("  data/materials.txt not found, skipped\n");
        return false;
    }
    for (auto& e : errors) std::printf("  materials: %s\n", e.c_str());
    CHECK(errors.empty());
    return true;
}

// The app's stack on the app's 16x8 chunk window, stepped in the app's order.
struct ShowRig {
    MaterialRegistry& mats;
    std::unique_ptr<Showcase> show;
    std::unique_ptr<WorldGen> gen;
    std::unique_ptr<World> world;
    std::unique_ptr<ThreadPool> pool;
    std::unique_ptr<Simulator> sim;
    std::unique_ptr<GasGrid> gas;
    std::unique_ptr<ParticleSystem> particles;
    std::unique_ptr<BodySystem> bodies;
    std::unique_ptr<ElectricSystem> electric;
    std::unique_ptr<Player> player;
    std::unique_ptr<Streamer> stream;
    PlayerInput in;
    bool withPlayer = false;

    explicit ShowRig(MaterialRegistry& m, bool overlay = true, std::uint64_t seed = 2026) : mats(m) {
        StreamParams sp; // 16 x 8 chunks, as in the app
        show = std::make_unique<Showcase>(mats);
        gen = std::make_unique<WorldGen>(mats, seed);
        if (overlay) gen->setOverlay(show.get());
        world = std::make_unique<World>(0, 0, sp.chunksW, sp.chunksH);
        if (overlay) world->gravity().set(show->flatRegions()); // #483: the top-down room
        pool = std::make_unique<ThreadPool>(4);
        sim = std::make_unique<Simulator>(*world, mats, pool.get(), seed);
        sim->params().reactions = false;
        gas = std::make_unique<GasGrid>(*world, mats, pool.get(), seed);
        sim->setGasExchange(gas.get());
        sim->params().ambientRate = 0.0f;
        particles = std::make_unique<ParticleSystem>(*world, mats, pool.get(), seed);
        sim->setParticles(particles.get());
        particles->setGas(gas.get());
        gas->setParticles(particles.get());
        bodies = std::make_unique<BodySystem>(*world, mats, particles.get(), seed);
        bodies->setGas(gas.get());
        electric = std::make_unique<ElectricSystem>(*world, mats, pool.get(), seed);
        player = std::make_unique<Player>(*world, mats, seed);
        player->setGas(gas.get());
        player->setBodies(bodies.get());
        player->setElectric(electric.get());
        player->inventory().reset(mats.size());
        player->inventory().creative = true;
        stream = std::make_unique<Streamer>(*world, mats, *gen, sp);
        stream->attach(sim.get(), gas.get(), particles.get(), bodies.get());
        stream->setPool(pool.get());
        stream->setElectric(electric.get());
    }
    void hookOverlay() {
        stream->setOverlayHook([this](const std::string& name) {
            if (name == show->name()) { gen->setOverlay(show.get()); world->gravity().set(show->flatRegions()); }
            else if (name.empty()) { gen->setOverlay(nullptr); world->gravity().clear(); }
            else return false;
            return true;
        });
    }
    void tick() {
        sim->tick();
        electric->step();
        bodies->step();
        particles->step();
        gas->step();
        if (withPlayer) {
            player->step(in);
            stream->follow(player->x(), player->centreY());
        }
    }
    long long conserved() const {
        const long long px = world->totalMass(mats) + particles->totalMass() + bodies->totalMass() + stream->storedMass();
        return (px - stream->generatedMass()) * kGasMassOne + gas->massFixed() + stream->storedGasFixed();
    }
};

} // namespace

TEST_CASE(showcase_overlay_is_pure_and_blends_into_the_terrain) {
    MaterialRegistry mats;
    if (!loadGameMaterials(mats)) return;
    Showcase show(mats);
    WorldGen plain(mats, 2026);
    WorldGen gen(mats, 2026);
    gen.setOverlay(&show);
    // generate() and cellAt() agree pixel for pixel, inside the building, across the
    // raster's edges and out in the generated terrain (negative coordinates included)
    const int coords[][2] = {{-3, -2}, {0, -1}, {5, -4}, {-18, 1}, {-17, 0}, {23, 2}, {24, -1}, {-5, 3}, {40, 1}};
    int mismatches = 0;
    for (const auto& c : coords) {
        Chunk ch;
        ch.coord = {c[0], c[1]};
        gen.generate(ch);
        for (int ly = 0; ly < kChunkSize; ++ly)
            for (int lx = 0; lx < kChunkSize; ++lx) {
                const Cell a = ch.at(lx, ly);
                const Cell b = gen.cellAt(ch.originX() + lx, ch.originY() + ly);
                mismatches += std::memcmp(&a, &b, sizeof a) != 0;
            }
    }
    CHECK_EQ(mismatches, 0);
    // a second instance builds the same raster
    Showcase again(mats);
    int diff = 0;
    for (int y = show.minY(); y <= show.maxY(); y += 3)
        for (int x = show.minX(); x <= show.maxX(); x += 3) {
            Cell a, b;
            const bool ia = show.cellAt(x, y, a), ib = again.cellAt(x, y, b);
            diff += ia != ib || (ia && std::memcmp(&a, &b, sizeof a) != 0);
        }
    CHECK_EQ(diff, 0);
    // the site is flat at y 0 and the hills come back further out
    CHECK_EQ(gen.surfaceY(0), 0);
    CHECK_EQ(gen.surfaceY(show.maxX()), 0);
    CHECK_EQ(gen.surfaceY(show.maxX() + 400), plain.surfaceY(show.maxX() + 400));
    CHECK_EQ(gen.surfaceY(show.minX() - 400), plain.surfaceY(show.minX() - 400));
    // walls are scene geometry, the loose blocks are settled, the rafts are live
    CHECK_EQ(gen.cellAt(-700, -50).aux, kAuxPinned);           // outer wall
    CHECK(gen.cellAt(-700, -50).material == mats.idOf("concrete"));
    CHECK_EQ(gen.cellAt(345, -170).aux, kAuxSettled);          // the iron weight in the demolition hall
    CHECK(gen.cellAt(345, -170).material == mats.idOf("iron"));
    CHECK_EQ(gen.cellAt(930, 8).aux, 0);                       // a raft on the pond
    CHECK(gen.cellAt(-160, -10).material == kEmpty);           // the lobby at the spawn point
    CHECK(gen.cellAt(-160, 0).material != kEmpty);             // ...standing on the floor
    for (const char* r : {"lobby", "gallery", "silo", "fluids", "cistern", "power", "furnace", "flue", "gas", "demolition", "cave", "course"})
        CHECK(show.room(r) != nullptr);
}

TEST_CASE(showcase_lights_up_holds_still_and_streams_with_mass_exact) {
    MaterialRegistry mats;
    if (!loadGameMaterials(mats)) return;
    ShowRig r(mats);
    r.stream->start(static_cast<int>(r.show->spawnX()), static_cast<int>(r.show->spawnY()));
    const long long m0 = r.conserved();
    int drift = 0;
    for (int i = 0; i < 180; ++i) {
        r.tick();
        drift += r.conserved() != m0;
    }
    CHECK_EQ(drift, 0);
    std::printf("  at the lobby: %d lamp px lit, %d circuits, %d live px, bodies %d\n", r.electric->stats().lit,
                r.electric->stats().components, r.electric->stats().nodes, r.bodies->stats().bodies);
    CHECK(r.electric->stats().lit > 200);            // the ceiling lamps of every room in the window
    CHECK_EQ(r.bodies->stats().bodies, 0);           // nothing loose comes down on its own
    // out to the course: the window shifts, the rafts wake up as floating bodies
    for (int k = 0; k < 4; ++k) {
        r.stream->shiftTo(r.stream->windowX() + 4, r.stream->windowY());
        for (int i = 0; i < 40; ++i) {
            r.tick();
            drift += r.conserved() != m0;
        }
    }
    CHECK_EQ(drift, 0);
    std::printf("  at the course: bodies %d (floating %d), %zu chunks asleep\n", r.bodies->stats().bodies,
                r.bodies->stats().floating, r.stream->storedChunks());
    CHECK(r.bodies->stats().bodies >= 2);
    CHECK(r.bodies->stats().floating >= 2);
    // and back: the lobby's lamps come back lit from the store
    r.stream->shiftTo(r.stream->windowX() - 16, r.stream->windowY());
    for (int i = 0; i < 30; ++i) {
        r.tick();
        drift += r.conserved() != m0;
    }
    CHECK_EQ(drift, 0);
    CHECK(r.electric->stats().lit > 200);
}

TEST_CASE(showcase_cutting_the_beam_drops_the_weight) {
    MaterialRegistry mats;
    if (!loadGameMaterials(mats)) return;
    ShowRig r(mats);
    r.stream->start(360, -150); // the demolition hall
    for (int i = 0; i < 20; ++i) r.tick();
    CHECK_EQ(r.bodies->stats().bodies, 0);
    const MaterialId iron = mats.idOf("iron");
    CHECK(r.world->material(345, -170) == iron);
    // saw through the beam under the iron weight (the dug pixels go nowhere: no mass check here)
    for (int y = -162; y <= -157; ++y)
        for (int x = 338; x <= 353; ++x)
            if (Cell* c = r.world->cell(x, y)) *c = Cell{};
    r.world->wakeRect(330, -180, 360, -150);
    int maxBodies = 0;
    for (int i = 0; i < 90; ++i) {
        r.tick();
        maxBodies = std::max(maxBodies, r.bodies->stats().bodies);
    }
    CHECK(maxBodies >= 1);
    CHECK(r.world->material(345, -170) != iron); // it fell
    int ironLow = 0;                             // ...into the tub (on its way down or frozen back into pixels)
    for (int y = -125; y <= -109; ++y)
        for (int x = 300; x <= 420; ++x) ironLow += r.world->material(x, y) == iron;
    std::printf("  iron pixels down in the tub: %d (bodies seen %d)\n", ironLow, maxBodies);
    CHECK(ironLow > 50 || r.bodies->stats().bodies >= 1);
}

TEST_CASE(showcase_player_climbs_the_stairs_to_the_gallery) {
    MaterialRegistry mats;
    if (!loadGameMaterials(mats)) return;
    ShowRig r(mats);
    r.stream->start(static_cast<int>(r.show->spawnX()), static_cast<int>(r.show->spawnY()));
    r.player->spawn(r.show->spawnX(), r.show->spawnY());
    r.withPlayer = true;
    r.in.move = 1.0f;
    float best = 1e9f;
    for (int i = 0; i < 600 && !(best <= -100.0f && r.player->x() > 80.0f); ++i) {
        r.tick();
        best = std::min(best, r.player->y());
    }
    std::printf("  feet at (%.0f, %.0f), highest %.0f\n", static_cast<double>(r.player->x()), static_cast<double>(r.player->y()),
                static_cast<double>(best));
    CHECK(best <= -100.0f); // up on the gallery floor (y -108)...
    CHECK(r.player->x() > 80.0f); // ...and walking on east past the top step
    // back west with a running jump over the stairwell: lands on the gallery's west part
    r.in.move = -1.0f;
    for (int i = 0; i < 20; ++i) r.tick();
    for (int i = 0; i < 160 && r.player->x() > 0.0f; ++i) {
        r.in.jump = r.player->x() < 69.0f && r.player->x() > 60.0f;
        r.tick();
    }
    r.in.jump = false;
    for (int i = 0; i < 20; ++i) r.tick();
    std::printf("  after the jump: feet at (%.0f, %.0f)\n", static_cast<double>(r.player->x()), static_cast<double>(r.player->y()));
    CHECK(r.player->x() < 25.0f);
    CHECK(r.player->y() <= -107.0f); // still up on the gallery floor
}

TEST_CASE(showcase_save_needs_and_restores_the_overlay) {
    MaterialRegistry mats;
    if (!loadGameMaterials(mats)) return;
    ShowRig a(mats);
    a.stream->start(static_cast<int>(a.show->spawnX()), static_cast<int>(a.show->spawnY()));
    for (int i = 0; i < 30; ++i) a.tick();
    std::vector<std::uint8_t> bytes;
    SaveExtras ex;
    CHECK(a.stream->saveToBytes(bytes, ex));
    // a plain world without a hook refuses it (it would generate the rest without the building)
    {
        ShowRig b(mats, false);
        SaveExtras got;
        std::string err;
        CHECK(!b.stream->loadFromBytes(bytes, got, &err));
        CHECK(!err.empty());
    }
    // with the hook it switches the generator to the showcase and carries on identically
    ShowRig c(mats, false);
    c.hookOverlay();
    SaveExtras got;
    std::string err;
    CHECK(c.stream->loadFromBytes(bytes, got, &err));
    CHECK(c.gen->overlay() == c.show.get());
    CHECK_EQ(c.world->hash(), a.world->hash());
    int diverged = -1;
    for (int i = 0; i < 30 && diverged < 0; ++i) {
        a.tick();
        c.tick();
        if (a.world->hash() != c.world->hash()) diverged = i;
    }
    CHECK_EQ(diverged, -1);
    // chunks first seen after the load still get the building
    c.stream->shiftTo(c.stream->windowX() + 12, c.stream->windowY());
    CHECK(c.world->material(830, -100) == mats.idOf("concrete")); // the jet tower
}
