// Phase 6 tests: the chunk codec, the procedural world, streaming (sleep/wake
// of chunks with every subsystem's state) and save files.
#include "test.h"

#include "sim/Bodies.h"
#include "sim/ChunkCodec.h"
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

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>

using namespace notita;

namespace {

// Own material table (same names the generator looks for), so retuning data/ doesn't break these.
const char* kMaterials = R"(
[bedrock]
kind = solid
density = 5000
anchor = true
color = #202020
[slate]
kind = solid
density = 2700
conductivity = 0.2
color = #707070
[basalt]
kind = solid
density = 2900
conductivity = 0.2
heat_capacity = 1.2
melt_point = 950
melts_into = magma
color = #403838
[iron]
kind = solid
density = 7800
conductivity = 0.6
color = #8890a0
[loam]
kind = solid
density = 1600
color = #6a4a2f
[turf]
kind = solid
density = 1300
color = #4f7d34
[sand]
kind = powder
density = 1600
color = #d0b060
[cinder_grit]
kind = powder
density = 1900
color = #504848
[water]
kind = liquid
density = 1000
spread = 6
conductivity = 0.3
boil_point = 100
boils_into = steam
latent_heat = 8
color = #3070c0
[lamp_oil]
kind = liquid
density = 820
spread = 5
color = #806020
[magma]
kind = liquid
density = 2800
viscosity = 0.92
spread = 2
conductivity = 0.25
heat_capacity = 1.2
spawn_temp = 1300
freeze_point = 750
freezes_into = basalt
color = #e8561c
[steam]
kind = gas
density = 0.6
heat_capacity = 0.5
condense_point = 70
condenses_into = water
color = #d0d8e0
[hydrogen]
kind = gas
density = 0.09
color = #e0e0ff
[murk]
kind = gas
density = 3.0
color = #405030
)";

MaterialRegistry loadMats(const char* extraFirst = nullptr) {
    MaterialRegistry mats;
    std::vector<std::string> errors;
    if (extraFirst) mats.loadFromString(extraFirst, &errors);
    mats.loadFromString(kMaterials, &errors);
    for (auto& e : errors) std::printf("  materials: %s\n", e.c_str());
    CHECK(errors.empty());
    return mats;
}

// A streamed world with every subsystem (bodies optional), stepped in the app's order.
struct Rig {
    MaterialRegistry mats;
    StreamParams sp;
    std::unique_ptr<WorldGen> gen;
    std::unique_ptr<World> world;
    std::unique_ptr<ThreadPool> pool;
    std::unique_ptr<Simulator> sim;
    std::unique_ptr<GasGrid> gas;
    std::unique_ptr<ParticleSystem> particles;
    std::unique_ptr<BodySystem> bodies;
    std::unique_ptr<Player> player;
    std::unique_ptr<Streamer> stream;
    PlayerInput in;
    bool withPlayer = false;

    Rig(bool withBodies, std::uint64_t seed = 11, const char* extraFirst = nullptr, int threads = 4)
        : mats(loadMats(extraFirst)) {
        sp.chunksW = 8;
        sp.chunksH = 5;
        gen = std::make_unique<WorldGen>(mats, seed);
        world = std::make_unique<World>(0, 0, sp.chunksW, sp.chunksH);
        pool = std::make_unique<ThreadPool>(threads);
        sim = std::make_unique<Simulator>(*world, mats, pool.get(), seed);
        sim->params().reactions = false;
        gas = std::make_unique<GasGrid>(*world, mats, pool.get(), seed);
        sim->setGasExchange(gas.get());
        sim->params().ambientRate = 0.0f;
        particles = std::make_unique<ParticleSystem>(*world, mats, pool.get(), seed);
        sim->setParticles(particles.get());
        particles->setGas(gas.get());
        gas->setParticles(particles.get());
        if (withBodies) {
            bodies = std::make_unique<BodySystem>(*world, mats, particles.get(), seed);
            bodies->setGas(gas.get());
        }
        player = std::make_unique<Player>(*world, mats, seed);
        player->setGas(gas.get());
        player->setBodies(bodies.get());
        player->inventory().reset(mats.size());
        stream = std::make_unique<Streamer>(*world, mats, *gen, sp);
        stream->attach(sim.get(), gas.get(), particles.get(), bodies.get());
        stream->setPool(pool.get());
    }
    void tick() {
        sim->tick();
        if (bodies) bodies->step();
        particles->step();
        gas->step();
        if (withPlayer) player->step(in);
    }
    // whole pixel units * kGasMassOne + fixed-point gas, live + sleeping
    long long mass() const {
        const long long px = world->totalMass(mats) + particles->totalMass() + (bodies ? bodies->totalMass() : 0) +
                             player->inventory().totalMass(mats) + stream->storedMass();
        return px * kGasMassOne + gas->massFixed() + stream->storedGasFixed();
    }
    // what must stay constant: everything minus the terrain generated so far
    long long conserved() const { return mass() - stream->generatedMass() * kGasMassOne; }
    SaveExtras extras() const {
        SaveExtras e;
        e.hasPlayer = withPlayer;
        e.player = player->state();
        e.creative = player->inventory().creative;
        e.capacity = player->inventory().capacity;
        e.charges = player->inventory().charges;
        const auto& counts = player->inventory().counts();
        for (std::size_t m = 0; m < counts.size(); ++m)
            if (counts[m]) e.inventory.emplace_back(static_cast<MaterialId>(m), counts[m]);
        return e;
    }
    void apply(const SaveExtras& e) {
        withPlayer = e.hasPlayer;
        player->setState(e.player);
        player->inventory().reset(mats.size());
        player->inventory().creative = e.creative;
        player->inventory().capacity = e.capacity;
        player->inventory().charges = e.charges;
        for (const auto& [m, n] : e.inventory) player->inventory().setCount(m, n);
    }
};

bool sameCells(const Chunk& a, const Chunk& b) { return std::memcmp(a.cells, b.cells, sizeof a.cells) == 0; }

} // namespace

// ------------------------------------------------------------- codec

TEST_CASE(world_rle_roundtrips_any_bytes) {
    Rng rng(3);
    for (int trial = 0; trial < 200; ++trial) {
        const std::size_t n = 1 + rng.below(700);
        std::vector<std::uint8_t> data(n);
        // mixes of runs (1..300 long) and noise
        for (std::size_t i = 0; i < n;) {
            const std::size_t len = std::min<std::size_t>(n - i, 1 + rng.below(trial % 3 == 0 ? 3 : 300));
            const std::uint8_t v = static_cast<std::uint8_t>(rng.below(trial % 2 ? 4 : 256));
            for (std::size_t k = 0; k < len; ++k) data[i + k] = rng.below(5) == 0 ? static_cast<std::uint8_t>(rng.u32()) : v;
            i += len;
        }
        std::vector<std::uint8_t> enc;
        rleEncode(data.data(), n, enc);
        std::vector<std::uint8_t> back(n, 0xEE);
        ByteReader r(enc.data(), enc.size());
        CHECK(rleDecode(r, back.data(), n));
        CHECK(r.atEnd());
        CHECK(back == data);
    }
    std::vector<std::uint8_t> zeros(4096, 0), enc;
    rleEncode(zeros.data(), zeros.size(), enc);
    CHECK(enc.size() <= 64); // 4096 equal bytes in 32 runs
}

TEST_CASE(world_codec_roundtrips_every_cell_field) {
    MaterialRegistry mats = loadMats();
    Chunk a, b;
    a.coord = b.coord = {-3, 7}; // negative coordinates on purpose
    Rng rng(9);
    for (Cell& c : a.cells) {
        c.material = static_cast<MaterialId>(rng.below(static_cast<std::uint32_t>(mats.size())));
        c.stamp = static_cast<std::uint16_t>(rng.u32());
        c.temperature = rng.unit() * 2000.0f - 100.0f;
        c.vx = static_cast<std::int16_t>(rng.u32());
        c.vy = static_cast<std::int16_t>(rng.u32());
        c.shade = static_cast<std::uint8_t>(rng.u32());
        c.flags = static_cast<std::uint8_t>(rng.u32());
        c.life = static_cast<std::uint8_t>(rng.u32());
        c.aux = static_cast<std::uint8_t>(rng.u32());
    }
    const auto blob = encodeCells(a);
    CHECK(decodeCells(blob, b));
    CHECK(sameCells(a, b));
    // a remap by name: every id shifted by one
    std::vector<MaterialId> remap(mats.size());
    for (std::size_t i = 0; i < remap.size(); ++i) remap[i] = static_cast<MaterialId>(i + 1);
    CHECK(decodeCells(blob, b, &remap));
    CHECK_EQ(b.cells[100].material, a.cells[100].material + 1);
    // corrupt data is refused, never read past the end
    auto bad = blob;
    bad.resize(bad.size() / 2);
    CHECK(!decodeCells(bad, b));
}

TEST_CASE(world_generated_chunks_compress_well) {
    MaterialRegistry mats = loadMats();
    WorldGen gen(mats, 5);
    std::size_t total = 0, chunks = 0;
    double genMs = 0;
    for (int cy = -2; cy < 8; ++cy)
        for (int cx = -4; cx < 4; ++cx) {
            Chunk c;
            c.coord = {cx, cy};
            const auto t0 = std::chrono::steady_clock::now();
            gen.generate(c);
            genMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            total += encodeCells(c).size();
            ++chunks;
        }
    const double mean = static_cast<double>(total) / chunks;
    std::printf("  generated chunk: %.0f bytes encoded on average (raw %zu), %.3f ms to generate\n", mean, sizeof(Cell) * kChunkCells,
                genMs / static_cast<double>(chunks));
    CHECK(mean < 3000.0); // > 20x smaller than the raw 64 KiB
}

// ------------------------------------------------------------- generator

TEST_CASE(world_gen_is_deterministic_seamless_and_varied) {
    MaterialRegistry mats = loadMats();
    WorldGen gen(mats, 21), gen2(mats, 21), other(mats, 22);
    Chunk a, b, c;
    a.coord = b.coord = c.coord = {-1, 2};
    gen.generate(a);
    gen2.generate(b);
    other.generate(c);
    CHECK(sameCells(a, b));
    CHECK(!sameCells(a, c));
    // generate() matches cellAt() pixel by pixel (borders line up for any chunk order)
    bool same = true;
    for (int ly = 0; ly < kChunkSize; ++ly)
        for (int lx = 0; lx < kChunkSize; ++lx) {
            const Cell want = gen.cellAt(a.originX() + lx, a.originY() + ly);
            same = same && std::memcmp(&want, &a.at(lx, ly), sizeof(Cell)) == 0;
        }
    CHECK(same);

    // across a wide strip: sky above the surface, ground below, caves, water, magma, pinned solids, biomes
    const MaterialId water = mats.idOf("water"), magma = mats.idOf("magma");
    long long air = 0, solid = 0, pinned = 0, waterN = 0, magmaN = 0, below = 0, caves = 0;
    bool biomes[3] = {};
    for (int x = -6000; x < 6000; x += 7) {
        const int s = gen.surfaceY(x);
        biomes[static_cast<int>(gen.biome(x))] = true;
        CHECK(gen.cellAt(x, s - 1).material == kEmpty);
        CHECK(gen.cellAt(x, s).material != kEmpty);
        for (int y = s; y < s + 900; y += 5) {
            const Cell cell = gen.cellAt(x, y);
            ++below;
            const Material& m = mats[cell.material];
            air += cell.material == kEmpty;
            caves += cell.material == kEmpty || m.kind == MaterialKind::Liquid || m.kind == MaterialKind::Gas;
            solid += m.kind == MaterialKind::Solid;
            pinned += m.kind == MaterialKind::Solid && cell.aux == kAuxPinned;
            waterN += cell.material == water;
            magmaN += cell.material == magma;
            if (cell.material == magma) CHECK(cell.temperature > 1000.0f);
        }
    }
    std::printf("  underground: %.1f%% caves (%.1f%% water, %.2f%% magma), %.1f%% solid\n", 100.0 * caves / below,
                100.0 * waterN / below, 100.0 * magmaN / below, 100.0 * solid / below);
    CHECK(solid == pinned);                 // terrain never falls as rigid bodies
    CHECK(caves > below / 20 && caves < below / 2);
    CHECK(waterN > 0);
    CHECK(magmaN > 0);
    CHECK(air > 0);
    CHECK(biomes[0] && biomes[1] && biomes[2]);
}

// ------------------------------------------------------------- streaming

TEST_CASE(world_stream_sleep_wake_roundtrip_is_exact) {
    Rig r(true);
    r.stream->start(0, r.gen->surfaceY(0));
    r.gas->params().enabled = true;
    // some of everything: sand thrown as particles, a loose block as a body, gas in the grid
    const int sx = 64, sy = r.gen->surfaceY(sx) - 30;
    Rng rng(1);
    scenes::fillRect(*r.world, r.mats.idOf("slate"), sx, sy, sx + 9, sy + 5, rng);
    for (int i = 0; i < 40; ++i) r.tick();
    r.gas->explode(sx + 40, r.gen->surfaceY(sx + 40), 14, 1.4f);
    for (int i = 0; i < 3; ++i) r.tick();
    const long long m0 = r.conserved();
    const std::uint64_t h0 = r.world->hash();
    const std::int64_t g0 = r.gas->massFixed();
    const std::size_t p0 = r.particles->count();
    const std::size_t b0 = r.bodies->bodies().size();
    std::printf("  before: %zu particles, %zu bodies, grid %lld\n", p0, b0, static_cast<long long>(g0));
    CHECK(p0 > 0);
    CHECK(b0 > 0);
    CHECK(g0 > 0);
    // all the way away (everything sleeps) and back
    const int cx0 = r.stream->windowX(), cy0 = r.stream->windowY();
    r.stream->shiftTo(cx0 + 40, cy0 + 3);
    CHECK_EQ(r.conserved(), m0);
    CHECK(r.stream->storedChunks() >= static_cast<std::size_t>(r.sp.chunksW * r.sp.chunksH));
    const long long genFar = r.stream->generatedMass();
    r.stream->shiftTo(cx0, cy0);
    CHECK_EQ(r.conserved(), m0);
    CHECK_EQ(r.stream->generatedMass(), genFar); // nothing new: the window came back from the store
    CHECK_EQ(r.gas->massFixed(), g0);
    CHECK_EQ(r.particles->count(), p0);
    CHECK_EQ(r.bodies->bodies().size(), b0);
    // the far window was stored too, so nothing that slept is in the store any more for this window
    for (int cy = cy0; cy < cy0 + r.sp.chunksH; ++cy)
        for (int cx = cx0; cx < cx0 + r.sp.chunksW; ++cx) CHECK(!r.stream->isStored(cx, cy));
    (void)h0; // the hash differs only by the dirty rects of the seams, the cells are what they were:
    r.stream->shiftTo(cx0 + 40, cy0 + 3);
    r.stream->shiftTo(cx0, cy0);
    CHECK_EQ(r.conserved(), m0);
    CHECK_EQ(r.stream->generatedMass(), genFar);
}

TEST_CASE(world_stream_keeps_mass_every_tick_while_moving) {
    Rig r(true);
    const int x0 = 0;
    r.stream->start(x0, r.gen->surfaceY(x0));
    const long long m0 = r.mass();
    bool ok = true;
    float fx = static_cast<float>(x0), fy = static_cast<float>(r.gen->surfaceY(x0));
    Rng rng(4);
    for (int t = 0; t < 420; ++t) {
        // the focus wanders right, down into the ground and back up to the left
        if (t < 200) fx += 3.1f;
        else if (t < 300) fy += 4.0f;
        else { fx -= 5.0f; fy -= 3.0f; }
        if (t % 50 == 10) r.gas->explode(static_cast<int>(fx), static_cast<int>(fy) + 20, 12, 1.3f);
        if (t % 70 == 5) {
            const int bx = static_cast<int>(fx) - 30, by = static_cast<int>(fy) - 60;
            scenes::fillRect(*r.world, r.mats.idOf("iron"), bx, by, bx + 6, by + 4, rng);
            scenes::fillRect(*r.world, r.mats.idOf("water"), bx + 20, by, bx + 40, by + 10, rng);
        }
        // the brush adds mass: re-base the expected total right after it
        const long long before = r.conserved();
        r.tick();
        r.stream->follow(fx, fy);
        const long long now = r.conserved();
        if (t % 70 == 5) continue; // fillRect replaced cells (mass changed on purpose before the tick)
        if (now != before && ok) {
            std::printf("  tick %d: mass %lld -> %lld\n", t, before, now);
            ok = false;
        }
    }
    CHECK(ok);
    CHECK(r.stream->stats().shifts >= 6);
    std::printf("  %d shifts, %d generated, %d woken, %d slept, store %zu chunks / %zu KiB, last shift %.2f ms\n",
                r.stream->stats().shifts, r.stream->stats().generated, r.stream->stats().woken, r.stream->stats().slept,
                r.stream->storedChunks(), r.stream->stats().storeBytes / 1024, r.stream->stats().lastShiftMs);
    (void)m0;
}

TEST_CASE(world_stream_sleeping_chunk_keeps_heat_and_moving_water) {
    Rig r(false);
    r.stream->start(0, 0);
    const int cx0 = r.stream->windowX(), cy0 = r.stream->windowY();
    // a hot iron block and water falling in a corner chunk
    const int x = cx0 * kChunkSize + 8, y = cy0 * kChunkSize + 4;
    Rng rng(2);
    scenes::fillRect(*r.world, r.mats.idOf("iron"), x, y, x + 7, y + 7, rng, 600.0f);
    for (int i = 0; i < 5; ++i) r.tick();
    Chunk before;
    before.coord = {cx0, cy0};
    std::memcpy(before.cells, r.world->chunk(cx0, cy0)->cells, sizeof before.cells);
    r.stream->shiftTo(cx0 + 4, cy0); // the corner chunk sleeps
    CHECK(r.stream->isStored(cx0, cy0));
    for (int i = 0; i < 30; ++i) r.tick(); // nothing in it may change while it sleeps
    r.stream->shiftTo(cx0, cy0);
    CHECK(sameCells(before, *r.world->chunk(cx0, cy0)));
    const Cell* hot = r.world->cell(x + 3, y + 3);
    CHECK(hot && hot->temperature > 400.0f);
}

TEST_CASE(world_stream_determinism_independent_of_thread_count) {
    // generation, encoding and decoding run on the pool during shifts; the result must not care
    std::uint64_t hashes[2] = {};
    std::int64_t gasMass[2] = {};
    const int threads[2] = {1, 6};
    for (int k = 0; k < 2; ++k) {
        Rig r(true, 11, nullptr, threads[k]);
        r.stream->start(0, r.gen->surfaceY(0));
        float fx = 0.0f, fy = static_cast<float>(r.gen->surfaceY(0));
        for (int t = 0; t < 160; ++t) {
            fx += 4.0f;
            fy += t < 80 ? 2.0f : -2.0f;
            if (t == 30) r.gas->explode(static_cast<int>(fx), static_cast<int>(fy), 12, 1.4f);
            r.tick();
            r.stream->follow(fx, fy);
        }
        hashes[k] = r.world->hash();
        gasMass[k] = r.gas->massFixed();
    }
    CHECK_EQ(hashes[0], hashes[1]);
    CHECK_EQ(gasMass[0], gasMass[1]);
}

TEST_CASE(world_stream_body_on_the_window_edge_survives_shifts) {
    // The window's edge is a wall for bodies (like for pixels); a body resting on it
    // keeps its mass when the window moves on, and nothing stays outside the window.
    Rig r(true);
    r.stream->start(0, 3000); // deep underground
    const int cx0 = r.stream->windowX(), cy0 = r.stream->windowY();
    const int x0 = cx0 * kChunkSize + 200, yb = (cy0 + r.sp.chunksH) * kChunkSize; // the window's bottom edge
    Rng rng(3);
    scenes::fillRect(*r.world, kEmpty, x0 - 20, yb - 120, x0 + 20, yb - 1, rng);
    scenes::fillRect(*r.world, r.mats.idOf("iron"), x0 - 5, yb - 110, x0 + 5, yb - 104, rng);
    const long long m0 = r.conserved();
    bool sawBody = false;
    const float fx = static_cast<float>((cx0 + r.sp.chunksW / 2) * kChunkSize);
    for (int t = 0; t < 240; ++t) {
        r.tick();
        r.stream->follow(fx, static_cast<float>((cy0 + r.sp.chunksH / 2) * kChunkSize));
        sawBody = sawBody || !r.bodies->bodies().empty();
        for (const RigidBody& body : r.bodies->bodies()) {
            float bx, by;
            BodySystem::centroid(body, bx, by);
            CHECK(by < static_cast<float>(yb));
        }
    }
    CHECK(sawBody);
    CHECK_EQ(r.stream->stats().strays, 0);
    CHECK_EQ(r.conserved(), m0);
    // the window moves down and back up: the block (body or frozen pixels) keeps its mass
    r.stream->shiftTo(cx0, cy0 + 2);
    CHECK_EQ(r.conserved(), m0);
    for (int i = 0; i < 60; ++i) r.tick();
    r.stream->shiftTo(cx0, cy0);
    CHECK_EQ(r.conserved(), m0);
    for (int i = 0; i < 60; ++i) r.tick();
    CHECK_EQ(r.conserved(), m0);
}

// ------------------------------------------------------------- saves

TEST_CASE(world_save_load_continues_identically) {
    // A runs on after saving; B loads the save. They must stay identical tick by tick.
    Rig a(false);
    a.stream->start(0, a.gen->surfaceY(0));
    for (int i = 0; i < 60; ++i) a.tick();
    a.stream->shiftTo(a.stream->windowX() + 3, a.stream->windowY() + 1); // some chunks asleep in the store
    a.gas->explode(a.stream->windowX() * kChunkSize + 200, a.gen->surfaceY(a.stream->windowX() * kChunkSize + 200), 14, 1.5f);
    for (int i = 0; i < 4; ++i) a.tick();
    CHECK(a.particles->count() > 0);
    std::vector<std::uint8_t> bytes;
    CHECK(a.stream->saveToBytes(bytes, a.extras()));
    std::printf("  save: %zu KiB for %zu chunks\n", bytes.size() / 1024, a.world->chunks().size() + a.stream->storedChunks());
    Rig b(false);
    SaveExtras ex;
    std::string err;
    CHECK(b.stream->loadFromBytes(bytes, ex, &err));
    CHECK(err.empty());
    CHECK_EQ(b.mass(), a.mass());
    CHECK_EQ(b.world->hash(), a.world->hash());
    CHECK(b.gas->fields().mass == a.gas->fields().mass);
    CHECK(b.gas->fields().heat == a.gas->fields().heat);
    CHECK(b.gas->fields().u == a.gas->fields().u && b.gas->fields().v == a.gas->fields().v);
    {
        int cellDiff = 0, rectDiff = 0, heatDiff = 0;
        for (const Chunk* ca : a.world->chunks()) {
            const Chunk* cb = b.world->chunk(ca->coord.x, ca->coord.y);
            if (!cb) { ++cellDiff; continue; }
            cellDiff += !sameCells(*ca, *cb);
            const DirtyRect ra = ca->nextRect.peek(), rb = cb->nextRect.peek();
            rectDiff += ra.x0 != rb.x0 || ra.y0 != rb.y0 || ra.x1 != rb.x1 || ra.y1 != rb.y1;
            heatDiff += ca->heatNext.load() != cb->heatNext.load();
        }
        CHECK_EQ(cellDiff, 0);
        CHECK_EQ(rectDiff, 0);
        CHECK_EQ(heatDiff, 0);
        CHECK_EQ(a.sim->tickCount(), b.sim->tickCount());
        CHECK_EQ(a.gas->tickCount(), b.gas->tickCount());
        CHECK_EQ(a.particles->tickCount(), b.particles->tickCount());
    }
    int firstDiff = -1;
    for (int i = 0; i < 90 && firstDiff < 0; ++i) {
        a.tick();
        b.tick();
        const bool same = a.world->hash() == b.world->hash() && a.gas->fields().mass == b.gas->fields().mass &&
                          a.gas->fields().u == b.gas->fields().u && a.gas->fields().heat == b.gas->fields().heat &&
                          a.particles->count() == b.particles->count();
        if (!same) {
            firstDiff = i;
            std::printf("  diverged at tick %d: world %d gas mass %d u %d v %d p %d heat %d particles %d\n", i,
                        a.world->hash() == b.world->hash(), a.gas->fields().mass == b.gas->fields().mass,
                        a.gas->fields().u == b.gas->fields().u, a.gas->fields().v == b.gas->fields().v,
                        a.gas->fields().p == b.gas->fields().p, a.gas->fields().heat == b.gas->fields().heat,
                        a.particles->count() == b.particles->count());
        }
    }
    CHECK_EQ(firstDiff, -1);
    CHECK_EQ(b.mass(), a.mass());
}

TEST_CASE(world_save_load_carries_bodies_player_and_inventory) {
    std::vector<std::uint8_t> bytes;
    long long massA = 0;
    std::size_t bodiesA = 0;
    PlayerState psA;
    {
        Rig a(true);
        a.stream->start(0, a.gen->surfaceY(0));
        const int x = 30, y = a.gen->surfaceY(30);
        a.withPlayer = true;
        a.player->spawn(static_cast<float>(x), static_cast<float>(y - 1));
        a.player->inventory().add(a.mats.idOf("sand"), 123);
        a.player->inventory().charges = 4;
        Rng rng(5);
        scenes::fillRect(*a.world, a.mats.idOf("slate"), x + 60, y - 80, x + 75, y - 70, rng);
        for (int i = 0; i < 12; ++i) a.tick();
        bodiesA = a.bodies->bodies().size();
        CHECK(bodiesA > 0);
        massA = a.mass();
        psA = a.player->state();
        CHECK(a.stream->saveToBytes(bytes, a.extras()));
        CHECK_EQ(a.mass(), massA); // saving doesn't disturb the live world
        CHECK_EQ(a.bodies->bodies().size(), bodiesA);
    }
    Rig b(true);
    SaveExtras ex;
    CHECK(b.stream->loadFromBytes(bytes, ex));
    b.apply(ex);
    CHECK(b.withPlayer);
    CHECK_EQ(b.bodies->bodies().size(), bodiesA);
    CHECK_EQ(b.mass(), massA);
    CHECK(b.player->x() == psA.x && b.player->y() == psA.y);
    CHECK_EQ(b.player->inventory().count(b.mats.idOf("sand")), 123);
    CHECK_EQ(b.player->inventory().charges, 4);
    for (int i = 0; i < 120; ++i) {
        b.tick();
        if (b.mass() != massA) { CHECK_EQ(b.mass(), massA); break; }
    }
}

TEST_CASE(world_save_remaps_materials_by_name_and_rejects_bad_files) {
    std::vector<std::uint8_t> bytes;
    std::size_t waterA = 0;
    {
        Rig a(false);
        a.stream->start(0, a.gen->surfaceY(0) + 300); // underground, around the water table
        waterA = a.world->count(a.mats.idOf("water")) ;
        CHECK(a.stream->saveToBytes(bytes, a.extras()));
    }
    // the same data with a new material first: every id moves by one
    Rig b(false, 11, "[glowmoss]\nkind = solid\ndensity = 900\ncolor = #40ff80\n");
    CHECK(b.mats.idOf("water") != loadMats().idOf("water"));
    SaveExtras ex;
    CHECK(b.stream->loadFromBytes(bytes, ex));
    CHECK_EQ(b.world->count(b.mats.idOf("water")), waterA);
    CHECK_EQ(b.world->count(b.mats.idOf("glowmoss")), 0u);

    std::string err;
    std::vector<std::uint8_t> bad = bytes;
    bad[0] = 'X';
    CHECK(!b.stream->loadFromBytes(bad, ex, &err));
    bad = bytes;
    bad[8] = 99; // version from the future
    CHECK(!b.stream->loadFromBytes(bad, ex, &err));
    bad = bytes;
    bad.resize(bytes.size() / 3);
    CHECK(!b.stream->loadFromBytes(bad, ex, &err));
    CHECK(!err.empty());
}
