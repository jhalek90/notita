// Phase 4 tests: splash particles, miscible liquids and rigid bodies.
#include "test.h"

#include "sim/Bodies.h"
#include "sim/GasGrid.h"
#include "sim/Material.h"
#include "sim/Particles.h"
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
heat_capacity = 1.0
color = #808080

[iron]
kind = solid
density = 7800
conductivity = 0.85
heat_capacity = 0.5
color = #8a8f96

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
miscible = ink
color = #3070c0

[ink]
kind = liquid
density = 1050
spread = 6
color = #202060

[oil]
kind = liquid
density = 800
viscosity = 0.1
spread = 5
color = #806020
)";

struct Fixture {
    MaterialRegistry mats;
    std::unique_ptr<World> world;
    std::unique_ptr<ThreadPool> pool;
    std::unique_ptr<Simulator> sim;
    std::unique_ptr<ParticleSystem> particles;
    std::unique_ptr<BodySystem> bodies;
    std::unique_ptr<GasGrid> gas;
    Rng rng{7};
    long long mass0 = 0;
    bool massOk = true;

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
    }
    void withParticles() { sim->setParticles(particles.get()); }
    void withBodies() {
        bodies = std::make_unique<BodySystem>(*world, mats, particles.get(), 3);
        sim->setParticles(particles.get());
    }
    void withGas() {
        gas = std::make_unique<GasGrid>(*world, mats, pool.get(), 5);
        sim->setGasExchange(gas.get());
        sim->params().ambientRate = 0.0f;
        particles->setGas(gas.get());
        gas->setParticles(particles.get());
        if (bodies) bodies->setGas(gas.get());
    }
    MaterialId id(const char* n) const { return mats.idOf(n); }
    void fill(const char* m, int x0, int y0, int x1, int y1, std::uint8_t aux = 0) {
        scenes::fillRect(*world, id(m), x0, y0, x1, y1, rng);
        if (aux)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    if (Cell* c = world->cell(x, y)) c->aux = aux;
    }
    void floor() { fill("base", 0, world->heightPx() - 8, world->widthPx() - 1, world->heightPx() - 1); }
    long long mass() const {
        return world->totalMass(mats) + particles->totalMass() + (bodies ? bodies->totalMass() : 0);
    }
    void startMass() { mass0 = mass(); }
    void tick() {
        sim->tick();
        if (bodies) bodies->step();
        particles->step();
        if (gas) gas->step();
        if (mass() != mass0) massOk = false;
    }
    void run(int ticks) { for (int i = 0; i < ticks; ++i) tick(); }
    MaterialId at(int x, int y) const { return world->material(x, y); }
    std::size_t countIn(MaterialId m, int x0, int y0, int x1, int y1) const {
        std::size_t n = 0;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) n += at(x, y) == m;
        return n;
    }
    int lowestRow(MaterialId m) const {
        for (int y = world->heightPx() - 1; y >= 0; --y)
            for (int x = 0; x < world->widthPx(); ++x)
                if (at(x, y) == m) return y;
        return -1;
    }
    int highestRow(MaterialId m) const {
        for (int y = 0; y < world->heightPx(); ++y)
            for (int x = 0; x < world->widthPx(); ++x)
                if (at(x, y) == m) return y;
        return -1;
    }
};

} // namespace

// ------------------------------------------------------------- particles

TEST_CASE(particle_round_trip_keeps_mass_and_hands_velocity_back) {
    Fixture f(2, 2);
    f.floor();
    f.withParticles();
    f.fill("water", 40, 60, 40, 60);
    f.startMass();
    CHECK(f.particles->launch(40, 60, 3.0f, -4.0f));
    CHECK_EQ(f.at(40, 60), kEmpty);
    CHECK_EQ(f.particles->count(), std::size_t{1});
    CHECK_EQ(f.mass(), f.mass0);
    // momentum leaves with the particle
    CHECK(std::abs(f.particles->momentumX() - 3.0) < 1e-5);
    CHECK(std::abs(f.particles->momentumY() + 4.0) < 1e-5);
    int settledAt = -1;
    float lastVy = 0;
    for (int t = 0; t < 200 && settledAt < 0; ++t) {
        if (!f.particles->particles().empty()) lastVy = f.particles->particles()[0].vy;
        f.tick();
        if (f.particles->count() == 0) settledAt = t;
    }
    CHECK(settledAt > 5);     // it flew for a while
    CHECK(f.massOk);
    CHECK_EQ(f.world->count(f.id("water")), std::size_t{1});
    // it landed to the right of where it started and came down fast; the cell got that speed
    bool found = false;
    for (const Chunk* ch : f.world->chunks())
        for (const Cell& c : ch->cells)
            if (c.material == f.id("water")) {
                found = true;
                CHECK(lastVy > 2.0f);
                CHECK(c.vy >= static_cast<std::int16_t>(lastVy * kVelOne) - 2 * kVelOne);
                CHECK(c.vx > 0);
            }
    CHECK(found);
}

TEST_CASE(splash_keeps_mass_and_budgets_momentum) {
    Fixture f(3, 4); // 192 x 256
    f.floor();
    f.withParticles();
    f.fill("stone", 40, 200, 150, 247);   // a basin block, the pool sits on it
    f.fill("water", 40, 190, 150, 199);   // 10 px deep pool
    f.fill("water", 90, 20, 100, 60);     // a column falling about 130 px
    f.startMass();
    int splashed = 0, maxParticles = 0;
    double in = 0, out = 0;
    for (int t = 0; t < 400; ++t) {
        f.tick();
        splashed += f.sim->stats().splashed;
        in += f.sim->stats().splashMomentumIn;
        out += f.sim->stats().splashMomentumOut;
        maxParticles = std::max(maxParticles, static_cast<int>(f.particles->count()));
    }
    CHECK(f.massOk);
    CHECK(splashed > 20);
    CHECK(maxParticles > 5);
    CHECK(in > 0.0);
    CHECK(out <= in * f.sim->params().restitution + 1e-3);
    // everything comes back down eventually
    f.run(600);
    CHECK(f.massOk);
    CHECK_EQ(f.particles->count(), std::size_t{0});
    CHECK_EQ(f.world->count(f.id("water")), std::size_t{111 * 10 + 11 * 41});
}

TEST_CASE(no_splash_without_a_particle_system) {
    Fixture f(3, 4);
    f.floor();
    f.fill("stone", 40, 200, 150, 247);
    f.fill("water", 40, 190, 150, 199);
    f.fill("water", 90, 20, 100, 60);
    f.startMass();
    int splashed = 0;
    for (int t = 0; t < 300; ++t) {
        f.tick();
        splashed += f.sim->stats().splashed;
    }
    CHECK_EQ(splashed, 0);
    CHECK(f.massOk);
}

TEST_CASE(particles_feel_the_gas_grid_and_blasts_throw_them) {
    Fixture f(3, 3);
    f.floor();
    f.withParticles();
    f.withGas();
    f.fill("stone", 20, 150, 170, 183);
    f.fill("water", 30, 140, 160, 149); // pool on the slab
    f.startMass();
    f.run(60);
    const long long gridBefore = f.gas->massFixed();
    f.gas->explode(95, 145, 12, 2.0f);
    f.tick();
    CHECK(f.particles->count() > 10);
    CHECK(f.massOk);
    // a strong draught in the grid bends the flight of a particle
    for (float& u : f.gas->fields().u) u = 1.5f;
    for (int t = 0; t < 8; ++t) f.tick();
    double px = 0;
    for (const Particle& p : f.particles->particles()) px += p.vx;
    CHECK(px > 0.0);
    CHECK(f.massOk);
    CHECK(f.gas->massFixed() == gridBefore);
    f.run(600);
    CHECK(f.massOk);
    CHECK_EQ(f.particles->count(), std::size_t{0});
}

// ------------------------------------------------------------- miscibility

TEST_CASE(miscible_liquids_blend_and_immiscible_ones_layer) {
    Fixture f(2, 2);
    f.floor();
    // two cups: ink on water (miscible, ink is a little denser), oil on water (not)
    f.fill("stone", 0, 60, 2, 119);
    f.fill("stone", 60, 60, 62, 119);
    f.fill("stone", 70, 60, 72, 119);
    f.fill("stone", 125, 60, 127, 119);
    f.fill("water", 3, 100, 59, 119);
    f.fill("ink", 3, 80, 59, 99);
    f.fill("water", 73, 100, 124, 119);
    f.fill("oil", 73, 80, 124, 99);
    f.startMass();
    f.run(900);
    CHECK(f.massOk);
    const std::size_t inkLow = f.countIn(f.id("ink"), 3, 100, 59, 119), inkAll = f.world->count(f.id("ink"));
    const std::size_t oilLow = f.countIn(f.id("oil"), 73, 100, 124, 119), oilAll = f.world->count(f.id("oil"));
    CHECK_EQ(inkAll, std::size_t{57 * 20});
    CHECK_EQ(oilAll, std::size_t{52 * 20});
    // blended: ink is down in the water half, water is up in the ink half, and the
    // band around the old interface is a real mixture
    CHECK(inkLow > inkAll / 8);
    CHECK(f.countIn(f.id("water"), 3, 80, 59, 99) > 57 * 20 / 8);
    const std::size_t bandInk = f.countIn(f.id("ink"), 3, 92, 59, 107), band = 57 * 16;
    CHECK(bandInk > band / 4 && bandInk < band * 3 / 4);
    // layered: oil stays on top
    CHECK(oilLow < oilAll / 20);
    // a blended pool settles down and sleeps (the swap count per pixel is capped)
    int active = 1;
    for (int t = 0; t < 3000 && active > 0; ++t) {
        f.tick();
        active = f.sim->stats().activeChunks;
    }
    if (active != 0)
        for (const Chunk* ch : f.world->chunks())
            if (ch->active())
            {
                std::printf("  still awake: chunk %d,%d rect %d,%d-%d,%d, %u cells\n", ch->coord.x, ch->coord.y, ch->rect.x0,
                            ch->rect.y0, ch->rect.x1, ch->rect.y1, ch->cellsProcessed);
                for (int y = ch->rect.y0 - 1; y <= ch->rect.y1 + 1; ++y) {
                    std::printf("   ");
                    for (int x = ch->rect.x0 - 1; x <= ch->rect.x1 + 1; ++x) {
                        const Cell& c = ch->at(std::clamp(x, 0, 63), std::clamp(y, 0, 63));
                        std::printf(" %d/%d", c.material, c.aux);
                    }
                    std::printf("\n");
                }
            }
    CHECK_EQ(active, 0);
    CHECK(f.massOk);
}

TEST_CASE(miscible_keys_parse_and_are_symmetric) {
    MaterialRegistry reg;
    std::vector<std::string> errors;
    reg.loadFromString(kMaterials, &errors);
    CHECK(errors.empty());
    CHECK(reg.miscible(reg.idOf("water"), reg.idOf("ink")));
    CHECK(reg.miscible(reg.idOf("ink"), reg.idOf("water")));
    CHECK(!reg.miscible(reg.idOf("water"), reg.idOf("oil")));
    CHECK(!reg.miscible(reg.idOf("water"), reg.idOf("water")));
    CHECK(reg.find("base")->anchor);
    CHECK(!reg.find("stone")->anchor);
    MaterialRegistry bad;
    errors.clear();
    bad.loadFromString("[a]\nkind = liquid\nmiscible = nope\n[b]\nkind = solid\nmiscible = a\n", &errors);
    CHECK_EQ(errors.size(), std::size_t{2});
    CHECK(!bad.miscible(bad.idOf("a"), bad.idOf("b")));
}

// ------------------------------------------------------------- bodies

TEST_CASE(cover_boxes_merges_runs_into_rectangles) {
    const std::uint8_t full[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    std::vector<BodySystem::Box> out;
    BodySystem::coverBoxes(full, 4, 3, out);
    CHECK_EQ(out.size(), std::size_t{1});
    if (!out.empty()) { CHECK_EQ(out[0].x1, 3); CHECK_EQ(out[0].y1, 2); }
    const std::uint8_t ell[9] = {1, 0, 0, 1, 0, 0, 1, 1, 1};
    BodySystem::coverBoxes(ell, 3, 3, out);
    CHECK_EQ(out.size(), std::size_t{2});
    int area = 0;
    for (const auto& b : out) area += (b.x1 - b.x0 + 1) * (b.y1 - b.y0 + 1);
    CHECK_EQ(area, 5);
}

TEST_CASE(unsupported_clump_becomes_a_body_and_lands) {
    Fixture f(3, 3); // 192 x 192
    f.floor();
    f.withBodies();
    f.bodies->params().shatterSpeed = 100.0f; // landing softly is tested here
    f.fill("stone", 60, 40, 71, 47); // 12 x 8 block in mid-air
    f.startMass();
    f.run(4);
    CHECK_EQ(f.bodies->bodies().size(), std::size_t{1});
    CHECK(f.massOk);
    CHECK_EQ(f.world->totalMass(f.mats), f.mass0 - 96); // the grid stamp is not counted twice
    f.run(400);
    CHECK(f.massOk);
    CHECK_EQ(f.bodies->bodies().size(), std::size_t{0}); // came to rest and froze back into pixels
    CHECK_EQ(f.world->count(f.id("stone")), std::size_t{96});
    CHECK_EQ(f.lowestRow(f.id("stone")), 183);
    CHECK(f.highestRow(f.id("stone")) >= 175);
    // settled pixels are marked and no cell still claims to be a body
    for (const Chunk* ch : f.world->chunks())
        for (const Cell& c : ch->cells)
            if (c.material == f.id("stone")) { CHECK_EQ(c.aux, kAuxSettled); CHECK(!(c.flags & kFlagBody)); }
}

TEST_CASE(anchored_pinned_and_resting_clumps_stay) {
    Fixture f(3, 3);
    f.floor();
    f.withBodies();
    f.fill("stone", 10, 40, 21, 47, kAuxPinned);    // scene geometry
    f.fill("stone", 0, 70, 11, 77);                 // touches the world edge
    f.fill("stone", 20, 100, 25, 183);              // a pillar standing on the anchor floor
    f.fill("stone", 49, 128, 49, 183, kAuxPinned);  // walls keep a sand bed from slumping
    f.fill("stone", 81, 128, 81, 183, kAuxPinned);
    f.fill("sand", 50, 128, 80, 183);
    f.fill("stone", 60, 120, 71, 127, kAuxSettled); // put down by physics earlier, resting on the sand
    f.fill("stone", 100, 60, 111, 67);              // the only live one: floats, so it falls
    f.startMass();
    f.run(6);
    CHECK_EQ(f.bodies->bodies().size(), std::size_t{1});
    f.run(400);
    CHECK(f.massOk);
    CHECK_EQ(f.countIn(f.id("stone"), 10, 40, 21, 47), std::size_t{96});
    CHECK_EQ(f.countIn(f.id("stone"), 0, 70, 11, 77), std::size_t{96});
    CHECK_EQ(f.countIn(f.id("stone"), 20, 100, 25, 183), std::size_t{6 * 84});
    CHECK_EQ(f.countIn(f.id("stone"), 60, 120, 71, 127), std::size_t{96});
    CHECK_EQ(f.countIn(f.id("stone"), 100, 60, 111, 67), std::size_t{0});
    CHECK_EQ(f.world->count(f.id("stone")), std::size_t{96 * 4 + 6 * 84 + 2 * 56});
}

TEST_CASE(settled_body_falls_again_when_its_support_goes) {
    Fixture f(3, 3);
    f.floor();
    f.withBodies();
    f.bodies->params().shatterSpeed = 100.0f;
    f.fill("stone", 80, 120, 91, 127, kAuxSettled); // sits on a pinned pillar
    f.fill("stone", 84, 128, 87, 183, kAuxPinned);
    f.startMass();
    f.run(60);
    CHECK_EQ(f.bodies->bodies().size(), std::size_t{0});
    CHECK_EQ(f.countIn(f.id("stone"), 80, 120, 91, 127), std::size_t{96});
    // knock the pillar out
    for (int y = 128; y <= 183; ++y)
        for (int x = 84; x <= 87; ++x) f.world->place(x, y, kEmpty, 0);
    f.mass0 -= 4 * 56;
    f.run(6);
    CHECK_EQ(f.bodies->bodies().size(), std::size_t{1});
    f.run(400);
    CHECK(f.massOk);
    CHECK_EQ(f.world->count(f.id("stone")), std::size_t{96});
    CHECK_EQ(f.lowestRow(f.id("stone")), 183);
}

TEST_CASE(body_shatters_on_a_hard_landing) {
    Fixture f(2, 6); // 128 x 384: a long drop
    f.floor();
    f.withBodies();
    f.fill("stone", 50, 10, 77, 29); // 28 x 20 slab
    f.startMass();
    int shattered = 0, maxBodies = 0;
    for (int t = 0; t < 600; ++t) {
        f.tick();
        shattered += f.bodies->stats().shattered;
        maxBodies = std::max(maxBodies, static_cast<int>(f.bodies->bodies().size()));
    }
    CHECK(f.massOk);
    CHECK(shattered >= 1);
    CHECK(maxBodies >= 2);
    f.run(600);
    CHECK(f.massOk);
    CHECK_EQ(f.bodies->bodies().size(), std::size_t{0});
    CHECK_EQ(f.particles->count(), std::size_t{0});
    CHECK_EQ(f.world->count(f.id("stone")), std::size_t{28 * 20});
    CHECK_EQ(f.lowestRow(f.id("stone")), 375);
    // it broke: the pieces spread wider than the slab was
    int x0 = 1000, x1 = -1;
    for (int y = 0; y < 384; ++y)
        for (int x = 0; x < 128; ++x)
            if (f.at(x, y) == f.id("stone")) { x0 = std::min(x0, x); x1 = std::max(x1, x); }
    CHECK(x1 - x0 + 1 > 30);
}

TEST_CASE(body_keeps_its_temperature_and_conducts_while_stamped) {
    Fixture f(3, 3);
    f.floor();
    f.withBodies();
    f.sim->params().heat = true;
    f.sim->params().ambientRate = 0.0f;
    f.bodies->params().shatterSpeed = 100.0f;
    f.fill("stone", 40, 160, 150, 183, kAuxPinned); // a cold slab to land on (over the anchor floor)
    scenes::fillRect(*f.world, f.id("iron"), 80, 100, 95, 111, f.rng, 600.0f); // hot iron block in the air
    f.startMass();
    const double e0 = f.world->thermalEnergy(f.mats) + f.particles->thermalEnergy() + f.bodies->thermalEnergy();
    f.run(6);
    CHECK_EQ(f.bodies->bodies().size(), std::size_t{1});
    if (!f.bodies->bodies().empty()) {
        double tmean = 0;
        for (const BodyPixel& p : f.bodies->bodies()[0].pixels) tmean += p.cell.temperature;
        tmean /= static_cast<double>(f.bodies->bodies()[0].pixels.size());
        CHECK(tmean > 450.0); // the body took its pixels' temperatures with it (the surface cools into the air)
    }
    f.run(500);
    CHECK(f.massOk);
    CHECK_EQ(f.bodies->bodies().size(), std::size_t{0});
    CHECK_EQ(f.world->count(f.id("iron")), std::size_t{16 * 12});
    // the slab under the iron warmed up, the iron cooled down
    float slabMax = 0.0f, ironMax = 0.0f;
    for (const Chunk* ch : f.world->chunks())
        for (int i = 0; i < kChunkCells; ++i) {
            const Cell& c = ch->cells[i];
            if (c.material == f.id("stone")) slabMax = std::max(slabMax, c.temperature);
            if (c.material == f.id("iron")) ironMax = std::max(ironMax, c.temperature);
        }
    CHECK(slabMax > 60.0f);
    CHECK(ironMax < 560.0f);
    const double e1 = f.world->thermalEnergy(f.mats) + f.particles->thermalEnergy() + f.bodies->thermalEnergy();
    CHECK(std::abs(e1 - e0) < 0.01 * std::abs(e0 - 20.0 * 0.25 * 192 * 192) + 50.0);
}

TEST_CASE(blast_kicks_a_body_and_shoves_loose_pixels_aside) {
    Fixture f(3, 3);
    f.floor();
    f.withBodies();
    f.withGas();
    f.bodies->params().shatterSpeed = 100.0f;
    f.fill("stone", 60, 150, 130, 183, kAuxPinned);
    f.fill("stone", 90, 120, 101, 127); // live block drops onto the slab and settles as a body first
    f.fill("water", 60, 140, 130, 149); // pool it lands in: the water is shoved out as particles
    f.startMass();
    int displaced = 0;
    for (int t = 0; t < 40; ++t) {
        f.tick();
        displaced += f.bodies->stats().displaced;
    }
    CHECK(displaced > 0);
    CHECK(f.massOk);
    CHECK_EQ(f.bodies->bodies().size(), std::size_t{1});
    f.gas->explode(80, 140, 12, 2.0f);
    f.bodies->explode(80, 140, 12, 2.0f);
    f.tick();
    CHECK_EQ(f.bodies->bodies().size(), std::size_t{1});
    if (!f.bodies->bodies().empty()) {
        const RigidBody& b = f.bodies->bodies()[0];
        CHECK(b.vx > 0.5f);  // thrown to the right, away from the blast
        CHECK(b.vy < 0.0f);  // and upwards
    }
    f.run(600);
    CHECK(f.massOk);
    CHECK_EQ(f.world->count(f.id("stone")) + f.particles->count() * 0, std::size_t{71 * 34 + 96});
}

TEST_CASE(determinism_with_particles_and_bodies) {
    auto hashAfter = [](int threads) {
        Fixture f(3, 4, threads, 11);
        f.floor();
        f.withBodies();
        f.fill("stone", 30, 200, 160, 247, kAuxPinned);
        f.fill("water", 30, 190, 160, 199);
        f.fill("water", 80, 20, 100, 60);
        f.fill("stone", 120, 60, 135, 71);
        f.fill("stone", 40, 90, 70, 100);
        f.startMass();
        f.run(350);
        CHECK(f.massOk);
        return f.world->hash() ^ (static_cast<std::uint64_t>(f.particles->count()) << 48) ^
               (static_cast<std::uint64_t>(f.bodies->bodies().size()) << 56);
    };
    const std::uint64_t a = hashAfter(1), b = hashAfter(3), c = hashAfter(8);
    CHECK_EQ(a, b);
    CHECK_EQ(a, c);
}
