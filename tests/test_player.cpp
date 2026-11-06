// Phase 5 tests: the player (movement, buoyancy, gas, damage), dig/place with
// an inventory that keeps mass, and the movement tools acting through the sim.
#include "test.h"

#include "sim/Bodies.h"
#include "sim/GasGrid.h"
#include "sim/Material.h"
#include "sim/Particles.h"
#include "sim/Player.h"
#include "sim/Scenes.h"
#include "sim/Simulator.h"
#include "sim/ThreadPool.h"
#include "sim/World.h"

#include <algorithm>
#include <cmath>
#include <memory>

using namespace notita;

namespace {

// Own material table so retuning data/ doesn't break these.
const char* kMaterials = R"(
[base]
kind = solid
density = 5000
anchor = true
color = #202020

[stone]
kind = solid
density = 2600
conductivity = 0.2
color = #808080

[wood]
kind = solid
density = 600
conductivity = 0.1
color = #806040

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
freeze_point = 0
freezes_into = ice
color = #3070c0

[ice]
kind = solid
density = 917
conductivity = 0.35
heat_capacity = 0.9
melt_point = 2
melts_into = water
color = #c0e0f0

[oil]
kind = liquid
density = 800
spread = 5
color = #806020

[steam]
kind = gas
density = 0.6
conductivity = 0.05
heat_capacity = 0.5
condense_point = 70
condenses_into = water
color = #d0d8e0
)";

struct Fixture {
    MaterialRegistry mats;
    std::unique_ptr<World> world;
    std::unique_ptr<ThreadPool> pool;
    std::unique_ptr<Simulator> sim;
    std::unique_ptr<ParticleSystem> particles;
    std::unique_ptr<BodySystem> bodies;
    std::unique_ptr<GasGrid> gas;
    std::unique_ptr<Player> player;
    Rng rng{7};
    long long mass0 = 0;
    bool tracking = false;
    bool massOk = true;
    PlayerInput in;

    Fixture(int chunksW, int chunksH, int threads = 4, std::uint64_t seed = 1) {
        std::vector<std::string> errors;
        mats.loadFromString(kMaterials, &errors);
        for (auto& e : errors) std::printf("  materials: %s\n", e.c_str());
        CHECK(errors.empty());
        world = std::make_unique<World>(0, 0, chunksW, chunksH);
        pool = std::make_unique<ThreadPool>(threads);
        sim = std::make_unique<Simulator>(*world, mats, pool.get(), seed);
        sim->params().heat = false;
        sim->params().reactions = false;
        particles = std::make_unique<ParticleSystem>(*world, mats, pool.get(), seed);
        player = std::make_unique<Player>(*world, mats, seed);
    }
    void withBodies() {
        bodies = std::make_unique<BodySystem>(*world, mats, particles.get(), 3);
        sim->setParticles(particles.get());
        player->setBodies(bodies.get());
        if (gas) bodies->setGas(gas.get());
    }
    void withGas() {
        gas = std::make_unique<GasGrid>(*world, mats, pool.get(), 5);
        sim->setGasExchange(gas.get());
        sim->params().ambientRate = 0.0f;
        particles->setGas(gas.get());
        gas->setParticles(particles.get());
        if (bodies) bodies->setGas(gas.get());
        player->setGas(gas.get());
    }
    MaterialId id(const char* n) const { return mats.idOf(n); }
    void fill(const char* m, int x0, int y0, int x1, int y1, float t = kAmbientTemp) {
        scenes::fillRect(*world, id(m), x0, y0, x1, y1, rng, t);
    }
    void pinAll() { scenes::pinRect(*world, mats, 0, 0, world->widthPx() - 1, world->heightPx() - 1); }
    void floor() { fill("base", 0, world->heightPx() - 8, world->widthPx() - 1, world->heightPx() - 1); }
    int ground() const { return world->heightPx() - 8; }
    // everything in fixed-point gas units: world + particles + bodies + inventory + grid
    long long mass() const {
        const long long px = world->totalMass(mats) + particles->totalMass() + (bodies ? bodies->totalMass() : 0) +
                             player->inventory().totalMass(mats);
        return px * kGasMassOne + (gas ? gas->massFixed() : 0);
    }
    void startMass() { mass0 = mass(); tracking = true; }
    void tick() {
        sim->tick();
        if (bodies) bodies->step();
        particles->step();
        if (gas) gas->step();
        player->step(in);
        in.usePressed = false;
        if (tracking && mass() != mass0) {
            if (massOk)
                std::printf("  mass %lld -> %lld (world %lld, particles %lld, bodies %lld, inventory %lld, grid %lld)\n", mass0, mass(),
                            world->totalMass(mats), particles->totalMass(), bodies ? bodies->totalMass() : 0,
                            player->inventory().totalMass(mats), gas ? gas->massFixed() : 0);
            massOk = false;
        }
    }
    void run(int ticks) { for (int i = 0; i < ticks; ++i) tick(); }
    MaterialId at(int x, int y) const { return world->material(x, y); }
    std::size_t countIn(MaterialId m, int x0, int y0, int x1, int y1) const {
        std::size_t n = 0;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) n += at(x, y) == m;
        return n;
    }
    void aim(float x, float y) { in.aimX = x; in.aimY = y; }
};

} // namespace

// ------------------------------------------------------------- movement

TEST_CASE(player_lands_walks_and_steps_up_but_not_walls) {
    Fixture f(4, 2); // 256 x 128
    f.floor();
    f.fill("stone", 80, 117, 125, 119);  // a 3 px step
    f.fill("stone", 160, 100, 165, 119); // a 20 px wall
    f.pinAll();
    f.player->spawn(40.5f, 60.0f);
    f.run(80);
    CHECK(f.player->grounded());
    CHECK(std::floor(f.player->y()) == static_cast<float>(f.ground()));
    f.in.move = 1.0f;
    f.run(60);
    CHECK(f.player->x() > 95.0f);
    CHECK(std::floor(f.player->y()) == 117.0f); // up on the step
    f.run(120);
    CHECK(f.player->x() < 160.0f - 3.0f); // stopped by the wall
    CHECK(f.player->x() > 150.0f);
    CHECK(f.player->health() == f.player->params().maxHealth);
}

TEST_CASE(player_jump_is_about_a_body_and_a_half) {
    Fixture f(2, 2);
    f.floor();
    f.player->spawn(64.5f, 100.0f);
    f.run(40);
    const float y0 = f.player->y();
    f.in.jump = true;
    float top = y0;
    for (int t = 0; t < 60; ++t) {
        f.tick();
        top = std::min(top, f.player->y());
    }
    CHECK(y0 - top > 20.0f);
    CHECK(y0 - top < 36.0f);
    CHECK(f.player->grounded()); // back down (holding jump doesn't bounce until released)
}

TEST_CASE(player_floats_in_water_and_sinks_in_oil) {
    for (int liquid = 0; liquid < 2; ++liquid) {
        Fixture f(2, 3); // 128 x 192
        f.floor();
        f.fill("stone", 0, 40, 15, 183);
        f.fill("stone", 112, 40, 127, 183);
        f.fill(liquid == 0 ? "water" : "oil", 16, 60, 111, 183);
        f.player->spawn(64.5f, 150.0f);
        f.run(400);
        if (liquid == 0) {
            // density 950 in 1000: it bobs at the surface, head out
            CHECK(f.player->y() - 20.0f < 64.0f);
            CHECK(f.player->y() > 60.0f);
            CHECK(f.player->stats().submerged > 0.5f);
        } else {
            CHECK(f.player->grounded()); // 950 in 800: on the bottom
            CHECK(f.player->y() >= 183.0f);
            // swimming up works against it
            f.in.jump = true;
            f.run(60);
            CHECK(f.player->y() < 150.0f);
        }
    }
}

TEST_CASE(player_is_pushed_by_the_gas_grid_wind) {
    float ys[2];
    for (int wind = 0; wind < 2; ++wind) {
        Fixture f(4, 4); // 256 x 256
        f.floor();
        f.withGas();
        f.player->spawn(128.5f, 120.0f);
        for (int t = 0; t < 40; ++t) {
            if (wind) {
                // an updraft through the middle of the grid, like hot air over a fire
                GasFields& g = f.gas->fields();
                for (int j = 0; j <= g.h; ++j)
                    for (int i = 24; i < 40; ++i) g.v[static_cast<std::size_t>(j) * g.w + i] = -1.6f;
            }
            f.tick();
        }
        ys[wind] = f.player->y();
        if (wind) CHECK(f.player->stats().windY < -2.0f);
    }
    CHECK(ys[1] < ys[0] - 30.0f); // the updraft held it up
}

TEST_CASE(cloak_glide_falls_slowly_and_rides_a_gentle_updraft) {
    float drop[3];
    for (int mode = 0; mode < 3; ++mode) { // 0 fall, 1 glide, 2 glide in a 2 px/tick updraft
        Fixture f(4, 4);
        f.floor();
        f.withGas();
        f.player->spawn(128.5f, 80.0f);
        f.in.jump = mode > 0;
        f.tick(); // (a held jump in the air never jumps)
        const float y0 = f.player->y();
        for (int t = 0; t < 60; ++t) {
            if (mode == 2) {
                GasFields& g = f.gas->fields();
                for (int j = 0; j <= g.h; ++j)
                    for (int i = 24; i < 40; ++i) g.v[static_cast<std::size_t>(j) * g.w + i] = -0.5f; // 2 px/tick up
            }
            f.tick();
        }
        drop[mode] = f.player->y() - y0;
        if (mode > 0) CHECK(f.player->gliding());
    }
    CHECK(drop[0] > 100.0f); // a plain fall goes a long way in a second
    CHECK(drop[1] > 20.0f && drop[1] < 80.0f); // gliding sinks about 1 px/tick
    CHECK(drop[2] < 0.0f);   // and a gentle updraft carries it up
}

TEST_CASE(player_is_hurt_by_heat_and_hard_landings) {
    Fixture f(2, 4); // 128 x 256
    f.floor();
    f.fill("stone", 70, 200, 80, 247, 400.0f); // a hot stone pillar (heat off: it stays hot)
    f.player->spawn(66.5f, 248.0f);            // right next to it
    f.run(60);
    CHECK(f.player->health() < 80.0f);
    CHECK(f.player->stats().hottest > 300.0f);

    Fixture g(2, 4);
    g.floor();
    g.player->spawn(30.5f, 248.0f);
    g.run(60);
    CHECK(g.player->health() == g.player->params().maxHealth);
    // a fall of about 220 px lands at over 9 px/tick
    g.player->spawn(30.5f, 25.0f);
    g.run(200);
    CHECK(g.player->grounded());
    CHECK(g.player->health() < 95.0f);
    CHECK(g.player->health() > 50.0f);
}

// ------------------------------------------------------------- dig / place / inventory

TEST_CASE(dig_and_place_keep_mass_with_every_system_on) {
    Fixture f(4, 3); // 256 x 192
    f.floor();
    f.withBodies();
    f.withGas();
    f.sim->params().heat = true;
    f.fill("stone", 20, 150, 60, 183);  // a stone block (pinned below)
    f.fill("sand", 100, 160, 140, 183); // a sand bed
    f.fill("stone", 160, 150, 163, 183);
    f.fill("stone", 210, 150, 213, 183);
    f.fill("water", 164, 160, 209, 183); // a pool
    f.pinAll();
    f.player->spawn(90.5f, 160.0f);
    f.run(30);
    f.startMass();
    f.in.tool = Tool::Dig;
    f.in.brushRadius = 4;
    const MaterialId sand = f.id("sand"), water = f.id("water"), stone = f.id("stone");
    const std::size_t sand0 = f.world->count(sand), stone0 = f.world->count(stone);
    f.in.use = true;
    f.aim(115.0f, 170.0f);
    f.run(30);
    f.aim(130.0f, 165.0f);
    f.run(30);
    const long long dugSand = f.player->inventory().count(sand);
    CHECK(dugSand > 20);
    f.aim(58.0f, 156.0f); // into the corner of the stone block (slower to dig)
    f.run(60);
    CHECK(f.player->inventory().count(stone) > 10);
    CHECK_EQ(static_cast<long long>(stone0 - f.world->count(stone)), f.player->inventory().count(stone));
    f.aim(140.0f, 180.0f); // the water is out of reach: nothing from it
    f.run(10);
    CHECK_EQ(f.player->inventory().count(water), 0);
    f.in.use = false;
    // place the sand back as a heap
    f.in.place = true;
    f.in.material = sand;
    f.aim(100.0f, 120.0f);
    f.run(30);
    f.in.place = false;
    CHECK(f.player->inventory().count(sand) < dugSand);
    f.run(120);
    CHECK(f.massOk);
    CHECK_EQ(static_cast<long long>(sand0) - static_cast<long long>(f.world->count(sand)) - f.particles->totalMass(),
             f.player->inventory().count(sand));
    // the player never placed anything inside itself
    for (int y = 0; y < f.world->heightPx(); ++y)
        for (int x = 0; x < f.world->widthPx(); ++x)
            if (f.player->covers(x, y)) CHECK(f.at(x, y) == kEmpty || f.at(x, y) == sand);
}

TEST_CASE(play_inventory_has_limits_and_creative_has_none) {
    Fixture f(4, 2);
    f.floor();
    f.fill("sand", 100, 100, 140, 119);
    f.player->spawn(80.5f, 120.0f);
    f.run(20);
    const MaterialId sand = f.id("sand"), wood = f.id("wood");
    // nothing to place yet
    f.in.place = true;
    f.in.material = wood;
    f.aim(60.0f, 105.0f);
    f.run(10);
    CHECK_EQ(f.world->count(wood), std::size_t{0});
    f.in.place = false;
    // a full inventory stops digging
    f.player->inventory().capacity = 10;
    f.in.use = true;
    f.aim(110.0f, 110.0f);
    f.run(20);
    CHECK_EQ(f.player->inventory().count(sand), 10);
    f.in.use = false;
    // creative: every material, nothing counted
    f.player->inventory().creative = true;
    f.in.place = true;
    f.aim(60.0f, 105.0f);
    f.run(10);
    CHECK(f.world->count(wood) > 20);
    CHECK_EQ(f.player->inventory().count(wood), 0);
    CHECK_EQ(f.player->inventory().count(sand), 10);
}

// ------------------------------------------------------------- movement tools

TEST_CASE(jet_pushes_real_gas_into_the_grid_and_lifts_the_player) {
    float rise[2];
    for (int fuel = 0; fuel < 2; ++fuel) {
        Fixture f(4, 4); // 256 x 256
        f.floor();
        f.withGas();
        f.pinAll();
        f.player->spawn(128.5f, static_cast<float>(f.ground()));
        f.run(10);
        if (fuel) f.player->inventory().add(f.id("water"), 200);
        f.startMass();
        const float y0 = f.player->y();
        f.in.tool = Tool::Jet;
        f.in.use = true;
        f.in.material = f.id("water");
        f.aim(128.0f, 400.0f); // straight down
        long long absorbed = 0;
        float top = y0;
        for (int t = 0; t < 60; ++t) {
            f.tick();
            absorbed += f.gas->stats().absorbed;
            top = std::min(top, f.player->y());
        }
        rise[fuel] = y0 - top;
        CHECK(f.massOk); // inventory water -> grid steam (-> rain), exactly
        if (fuel) {
            CHECK_EQ(f.player->stats().jetMassTotal, 60);
            CHECK_EQ(f.player->inventory().count(f.id("water")), 140);
            CHECK(absorbed >= 59); // the steam went through the grid's intake (the last one is counted next step)
        } else {
            CHECK_EQ(f.player->stats().jetMassTotal, 0);
        }
    }
    CHECK(rise[0] < 1.0f);  // no propellant, no thrust
    CHECK(rise[1] > 40.0f); // it flies
}

TEST_CASE(blast_jump_goes_through_the_explosion_path) {
    Fixture f(4, 3); // 256 x 192
    f.floor();
    f.withBodies();
    f.withGas();
    f.fill("sand", 0, 170, 255, 183);
    f.pinAll();
    f.player->spawn(128.5f, 170.0f);
    f.run(20);
    CHECK(f.player->grounded());
    f.startMass();
    const float y0 = f.player->y();
    const int charges = f.player->inventory().charges;
    f.in.tool = Tool::Blast;
    f.aim(128.0f, 172.0f); // at the feet
    f.in.usePressed = true;
    f.tick();
    CHECK(f.player->stats().blasted);
    CHECK_EQ(f.player->inventory().charges, charges - 1);
    float top = y0;
    int flying = 0;
    for (int t = 0; t < 60; ++t) {
        f.tick();
        top = std::min(top, f.player->y());
        flying = std::max(flying, static_cast<int>(f.particles->count()));
    }
    CHECK(y0 - top > 30.0f); // launched
    CHECK(flying > 20);       // the sand flew as particles: the same blast as the X key
    CHECK(f.massOk);
    // out of charges: nothing happens
    f.player->inventory().charges = 0;
    f.in.usePressed = true;
    f.tick();
    CHECK(!f.player->stats().blasted);
}

TEST_CASE(rafts_float_by_density_and_carry_the_player) {
    for (int heavy = 0; heavy < 2; ++heavy) {
        Fixture f(4, 3); // 256 x 192
        f.floor();
        f.withBodies();
        f.fill("stone", 0, 60, 19, 183);
        f.fill("stone", 236, 60, 255, 183);
        f.fill("water", 20, 100, 235, 183);
        f.pinAll();
        const MaterialId m = f.id(heavy ? "stone" : "wood");
        f.player->spawn(10.5f, 60.0f);
        f.run(10);
        f.player->inventory().add(m, 400);
        f.startMass();
        f.in.tool = Tool::Raft;
        f.in.material = m;
        f.aim(60.0f, 80.0f);
        f.in.usePressed = true;
        f.tick();
        CHECK_EQ(f.player->inventory().count(m), 400 - 48 * 6);
        CHECK_EQ(f.bodies->bodies().size(), std::size_t{1});
        f.run(400);
        CHECK(f.massOk);
        if (!heavy) {
            CHECK_EQ(f.bodies->bodies().size(), std::size_t{1}); // a floating body stays a body
            const RigidBody& b = f.bodies->bodies()[0];
            // stand on it: it carries the player
            f.player->spawn(b.x + 0.5f, b.y - 4.0f);
            f.run(120);
            CHECK(f.player->grounded());
            CHECK(f.player->groundBody() == f.bodies->bodies()[0].id);
            CHECK(f.bodies->bodies()[0].y < 106.0f); // still afloat with the player on it
        } else {
            // stone sinks to the bottom
            int top = -1;
            for (int y = 100; y < 184 && top < 0; ++y)
                for (int x = 20; x < 236; ++x)
                    if (f.at(x, y) == m) { top = y; break; }
            float bodyY = 0;
            for (const RigidBody& b : f.bodies->bodies()) bodyY = std::max(bodyY, b.y);
            CHECK(top > 170 || bodyY > 170.0f);
        }
    }
}

TEST_CASE(grapple_hangs_the_player_and_reels_in) {
    Fixture f(4, 3); // 256 x 192
    f.floor();
    f.fill("stone", 0, 20, 255, 27);
    f.pinAll();
    f.player->spawn(128.5f, static_cast<float>(f.ground()));
    f.run(10);
    f.in.tool = Tool::Grapple;
    f.aim(128.0f, 0.0f);
    f.in.usePressed = true;
    f.tick();
    CHECK(f.player->roped());
    CHECK(std::abs(f.player->ropeY() - 27.5f) < 1.0f);
    const float c0 = f.player->centreY();
    f.in.jump = true; // reel in
    f.run(60);
    f.in.jump = false;
    CHECK(f.player->centreY() < c0 - 60.0f);
    CHECK(!f.player->grounded());
    f.run(30); // hangs there
    CHECK(f.player->centreY() < c0 - 60.0f);
    CHECK(f.player->centreY() - f.player->ropeY() <= f.player->ropeLength() + 1.0f);
    // the anchor is dug away: the rope comes loose
    f.fill("stone", 0, 20, 255, 27); // (refill to keep things tidy, then remove the anchor pixel)
    f.world->place(static_cast<int>(f.player->ropeX()), static_cast<int>(f.player->ropeY()), kEmpty, 0);
    f.tick();
    CHECK(!f.player->roped());
    f.run(120);
    CHECK(f.player->grounded());
}

TEST_CASE(freeze_turns_water_into_ice_you_can_stand_on) {
    Fixture f(4, 3); // 256 x 192
    f.floor();
    f.withBodies();
    f.sim->params().heat = true;
    f.fill("stone", 0, 100, 19, 183);
    f.fill("stone", 236, 60, 255, 183);
    f.fill("water", 20, 100, 235, 183);
    f.pinAll();
    f.player->spawn(10.5f, 100.0f);
    f.run(10);
    f.startMass();
    f.in.tool = Tool::Freeze;
    f.in.brushRadius = 4;
    f.in.use = true;
    for (int x = 22; x <= 60; x += 2) {
        f.aim(static_cast<float>(x), 102.0f);
        f.run(4);
    }
    f.in.use = false;
    const MaterialId ice = f.id("ice");
    std::size_t frozen = f.countIn(ice, 20, 95, 70, 110);
    for (const RigidBody& b : f.bodies->bodies())
        for (const BodyPixel& p : b.pixels) frozen += p.cell.material == ice;
    CHECK(frozen > 80);
    CHECK(f.massOk);
    // walk out onto it
    f.in.move = 1.0f;
    f.run(40);
    f.in.move = 0.0f;
    f.run(20);
    CHECK(f.player->x() > 30.0f);
    CHECK(f.player->y() < 106.0f);   // standing on the ice, not swimming in the water
    CHECK(f.player->stats().submerged < 0.3f);
    CHECK(f.player->health() > 90.0f); // the tool doesn't make ice cold enough to hurt
}

TEST_CASE(player_and_tools_are_deterministic_across_thread_counts) {
    std::uint64_t hashes[2];
    float pos[2];
    const int threads[2] = {1, 6};
    for (int r = 0; r < 2; ++r) {
        Fixture f(4, 3, threads[r]);
        f.floor();
        f.withBodies();
        f.withGas();
        f.fill("sand", 0, 160, 255, 183);
        f.fill("water", 150, 120, 200, 159);
        f.player->spawn(60.5f, 160.0f);
        f.player->inventory().add(f.id("water"), 50);
        f.in.tool = Tool::Dig;
        f.in.use = true;
        f.aim(80.0f, 165.0f);
        f.run(30);
        f.in.tool = Tool::Jet;
        f.in.material = f.id("water");
        f.aim(60.0f, 300.0f);
        f.run(30);
        f.in.tool = Tool::Blast;
        f.in.use = false;
        f.aim(90.0f, 165.0f);
        f.in.usePressed = true;
        f.in.move = 1.0f;
        f.run(120);
        hashes[r] = f.world->hash();
        pos[r] = f.player->x() * 1000.0f + f.player->y();
    }
    CHECK(hashes[0] == hashes[1]);
    CHECK(pos[0] == pos[1]);
}

TEST_CASE(playground_scene_runs_with_a_player_and_keeps_mass) {
    MaterialRegistry mats = MaterialRegistry::loadDefault();
    World world(0, 0, 16, 8);
    ThreadPool pool(4);
    Simulator sim(world, mats, &pool, 9);
    GasGrid gas(world, mats, &pool, 9);
    sim.setGasExchange(&gas);
    sim.params().ambientRate = 0.0f;
    ParticleSystem particles(world, mats, &pool, 9);
    BodySystem bodies(world, mats, &particles, 9);
    sim.setParticles(&particles);
    particles.setGas(&gas);
    gas.setParticles(&particles);
    bodies.setGas(&gas);
    Player player(world, mats, 9);
    player.setGas(&gas);
    player.setBodies(&bodies);
    float sx = 0, sy = 0;
    scenes::playground(world, mats, 9, &sx, &sy);
    player.spawn(sx, sy);
    auto mass = [&] {
        return (world.totalMass(mats) + particles.totalMass() + bodies.totalMass() + player.inventory().totalMass(mats)) * kGasMassOne +
               gas.massFixed();
    };
    long long m0 = 0;
    PlayerInput in;
    in.move = 1.0f;
    in.tool = Tool::Dig;
    bool ok = true;
    for (int t = 0; t < 400; ++t) {
        if (t == 30) m0 = mass();
        in.use = t > 120;
        in.aimX = player.x() + 20.0f;
        in.aimY = player.centreY();
        sim.tick();
        bodies.step();
        particles.step();
        gas.step();
        player.step(in);
        if (t >= 30 && mass() != m0) ok = false;
    }
    CHECK(ok);
    CHECK(std::isfinite(player.x()) && std::isfinite(player.y()));
    CHECK(player.x() > sx + 60.0f);                  // it got going (walked, dug into the hill)
    CHECK(player.inventory().total() > 0);
    CHECK(bodies.stats().floating >= 2);             // the planks float as rafts
}
