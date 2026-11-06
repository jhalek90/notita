#include "Streamer.h"

#include "Simulator.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace notita {

namespace {

constexpr char kMagic[8] = {'N', 'O', 'T', 'I', 'T', 'A', 'S', 'V'};

int floorDiv(int v, int d) { return v >= 0 ? v / d : -((-v + d - 1) / d); }

void writeCell(ByteWriter& w, const Cell& c) { w.raw(&c, sizeof(Cell)); }
Cell readCell(ByteReader& r, const std::vector<MaterialId>& remap) {
    Cell c;
    r.raw(&c, sizeof(Cell));
    c.material = c.material < remap.size() ? remap[c.material] : kEmpty;
    return c;
}

void writeParticle(ByteWriter& w, const Particle& p) {
    w.f32(p.x); w.f32(p.y); w.f32(p.vx); w.f32(p.vy);
    writeCell(w, p.cell);
    w.u16(p.age); w.u16(p.rest);
}
Particle readParticle(ByteReader& r, const std::vector<MaterialId>& remap) {
    Particle p;
    p.x = r.f32(); p.y = r.f32(); p.vx = r.f32(); p.vy = r.f32();
    p.cell = readCell(r, remap);
    p.age = r.u16(); p.rest = r.u16();
    return p;
}

void writeBody(ByteWriter& w, const BodyState& b) {
    w.f32(b.x); w.f32(b.y); w.f32(b.c); w.f32(b.s); w.f32(b.vx); w.f32(b.vy); w.f32(b.w);
    w.i32(b.cooldown); w.i32(b.asleep); w.u8(b.floats ? 1 : 0);
    w.u32(static_cast<std::uint32_t>(b.pixels.size()));
    for (const BodyPixel& p : b.pixels) {
        w.u16(static_cast<std::uint16_t>(p.lx));
        w.u16(static_cast<std::uint16_t>(p.ly));
        writeCell(w, p.cell);
    }
}
BodyState readBody(ByteReader& r, const std::vector<MaterialId>& remap) {
    BodyState b;
    b.x = r.f32(); b.y = r.f32(); b.c = r.f32(); b.s = r.f32(); b.vx = r.f32(); b.vy = r.f32(); b.w = r.f32();
    b.cooldown = r.i32(); b.asleep = r.i32(); b.floats = r.u8() != 0;
    const std::uint32_t n = r.u32();
    if (!r.ok() || n > (1u << 20)) return b;
    b.pixels.resize(n);
    for (BodyPixel& p : b.pixels) {
        p.lx = static_cast<std::int16_t>(r.u16());
        p.ly = static_cast<std::int16_t>(r.u16());
        p.cell = readCell(r, remap);
    }
    return b;
}

long long bodyMass(const BodyState& b, const MaterialRegistry& mats) {
    long long m = 0;
    for (const BodyPixel& p : b.pixels) m += mats[p.cell.material].mass;
    return m;
}

long long cellsMass(const Chunk& c, const MaterialRegistry& mats) {
    long long m = 0;
    for (const Cell& cell : c.cells)
        if (!(cell.flags & kFlagBody)) m += mats[cell.material].mass;
    return m;
}

} // namespace

std::size_t StoredChunk::bytes() const {
    std::size_t n = cells.size() + gas.size() + particles.size() * sizeof(Particle);
    for (const BodyState& b : bodies) n += b.pixels.size() * sizeof(BodyPixel) + sizeof(BodyState);
    return n;
}

Streamer::Streamer(World& world, const MaterialRegistry& mats, const WorldGen& gen, StreamParams params)
    : world_(world), mats_(mats), gen_(gen), params_(params) {}

void Streamer::attach(Simulator* sim, GasGrid* gas, ParticleSystem* particles, BodySystem* bodies) {
    sim_ = sim;
    gas_ = gas;
    particles_ = particles;
    bodies_ = bodies;
}

void Streamer::parallel(int n, const std::function<void(int)>& fn) {
    if (pool_ && n > 1) pool_->parallelFor(n, fn);
    else for (int i = 0; i < n; ++i) fn(i);
}

void Streamer::forgetStore() {
    store_.clear();
    storedMass_ = 0;
    storedGas_ = 0;
    stats_.storeBytes = 0;
    stats_.storeChunks = 0;
}

void Streamer::reset(int cx0, int cy0) {
    if (bodies_) bodies_->clear();
    if (particles_) particles_->clear();
    if (gas_) gas_->clear();
    std::vector<ChunkCoord> all;
    for (const Chunk* c : world_.chunks()) all.push_back(c->coord);
    for (const ChunkCoord& c : all) world_.removeChunk(c.x, c.y);
    forgetStore();
    generatedMass_ = 0;
    wcx_ = cx0;
    wcy_ = cy0;
    if (gas_) gas_->rebase(cx0 * kChunkSize, cy0 * kChunkSize);
    for (int cy = cy0; cy < cy0 + params_.chunksH; ++cy)
        for (int cx = cx0; cx < cx0 + params_.chunksW; ++cx) world_.createChunk(cx, cy);
}

void Streamer::start(int x, int y) {
    const int cx0 = floorDiv(x, kChunkSize) - params_.chunksW / 2;
    const int cy0 = floorDiv(y, kChunkSize) - params_.chunksH / 2;
    reset(cx0, cy0);
    const auto& chunks = world_.chunks();
    std::vector<long long> mass(chunks.size(), 0);
    parallel(static_cast<int>(chunks.size()), [&](int i) {
        gen_.generate(*chunks[i]);
        mass[i] = cellsMass(*chunks[i], mats_);
    });
    for (long long m : mass) generatedMass_ += m;
    stats_.generated += static_cast<int>(chunks.size());
    world_.wakeAll();
}

bool Streamer::follow(float x, float y) {
    const int fcx = floorDiv(static_cast<int>(std::floor(x)), kChunkSize);
    const int fcy = floorDiv(static_cast<int>(std::floor(y)), kChunkSize);
    const int ccx = wcx_ + params_.chunksW / 2, ccy = wcy_ + params_.chunksH / 2;
    if (std::abs(fcx - ccx) <= params_.slackX && std::abs(fcy - ccy) <= params_.slackY) return false;
    shiftTo(fcx - params_.chunksW / 2, fcy - params_.chunksH / 2);
    return true;
}

StoredChunk Streamer::encodeLive(const Chunk& c, bool clearGas) {
    StoredChunk sc;
    sc.cells = encodeCells(c);
    sc.mass = cellsMass(c, mats_);
    if (gas_) {
        GasGrid::ChunkCells g;
        if (gas_->exportChunk(c.coord.x, c.coord.y, g, clearGas)) {
            sc.gasMass = g.massFixed();
            sc.gas = encodeGas(g);
        }
    }
    sc.next = c.nextRect.peek();
    sc.heatNext = c.heatNext.load(std::memory_order_relaxed);
    return sc;
}

void Streamer::sleepChunk(int cx, int cy, StoredChunk&& encoded) {
    Chunk* c = world_.chunk(cx, cy);
    if (!c) return;
    StoredChunk sc = std::move(encoded);
    if (gas_) { // the grid side is serial (it clears the cells it hands over)
        GasGrid::ChunkCells g;
        if (gas_->exportChunk(cx, cy, g, true)) {
            sc.gasMass = g.massFixed();
            sc.gas = encodeGas(g);
        }
    }
    const int x0 = c->originX(), y0 = c->originY();
    if (particles_) {
        particles_->extractIn(x0, y0, x0 + kChunkSize, y0 + kChunkSize, sc.particles);
        for (const Particle& p : sc.particles) sc.mass += mats_[p.cell.material].mass;
    }
    if (bodies_) {
        bodies_->extractIn(x0, y0, x0 + kChunkSize, y0 + kChunkSize, sc.bodies);
        for (const BodyState& b : sc.bodies) sc.mass += bodyMass(b, mats_);
    }
    storedMass_ += sc.mass;
    storedGas_ += sc.gasMass;
    stats_.storeBytes += sc.bytes();
    store_[chunkKey(cx, cy)] = std::move(sc);
    world_.removeChunk(cx, cy);
    ++stats_.slept;
}

void Streamer::wakeChunk(int cx, int cy, bool decoded) {
    Chunk* c = world_.createChunk(cx, cy);
    const int ox = c->originX(), oy = c->originY();
    auto it = store_.find(chunkKey(cx, cy));
    if (it == store_.end()) {
        if (!decoded) gen_.generate(*c);
        generatedMass_ += cellsMass(*c, mats_);
        world_.wakeRect(ox, oy, ox + kChunkMask, oy + kChunkMask); // let fresh terrain settle
        ++stats_.generated;
    } else {
        StoredChunk& sc = it->second;
        if (!decoded) decodeCells(sc.cells, *c);
        if (!sc.next.empty()) c->nextRect.include(sc.next.x0, sc.next.y0, sc.next.x1, sc.next.y1);
        c->heatNext.store(sc.heatNext, std::memory_order_relaxed);
        if (gas_ && !sc.gas.empty()) {
            GasGrid::ChunkCells g;
            if (decodeGas(sc.gas, g, gas_->speciesCount())) gas_->importChunk(cx, cy, g);
        }
        if (particles_ && !sc.particles.empty()) particles_->add(sc.particles.data(), sc.particles.size());
        if (bodies_)
            for (const BodyState& b : sc.bodies) bodies_->restore(b);
        storedMass_ -= sc.mass;
        storedGas_ -= sc.gasMass;
        stats_.storeBytes -= std::min(stats_.storeBytes, sc.bytes());
        store_.erase(it);
        ++stats_.woken;
    }
}

void Streamer::shiftTo(int cx0, int cy0) {
    if (cx0 == wcx_ && cy0 == wcy_) return;
    const auto t0 = std::chrono::steady_clock::now();
    if (bodies_) bodies_->unstampAll();
    auto inside = [&](int cx, int cy) {
        return cx >= cx0 && cy >= cy0 && cx < cx0 + params_.chunksW && cy < cy0 + params_.chunksH;
    };
    std::vector<ChunkCoord> leaving;
    for (const Chunk* c : world_.chunks())
        if (!inside(c->coord.x, c->coord.y)) leaving.push_back(c->coord);
    {
        std::vector<StoredChunk> enc(leaving.size());
        parallel(static_cast<int>(leaving.size()), [&](int i) {
            const Chunk& c = *world_.chunk(leaving[i].x, leaving[i].y);
            enc[i].cells = encodeCells(c);
            enc[i].mass = cellsMass(c, mats_);
            enc[i].next = c.nextRect.peek();
            enc[i].heatNext = c.heatNext.load(std::memory_order_relaxed);
        });
        for (std::size_t i = 0; i < leaving.size(); ++i) sleepChunk(leaving[i].x, leaving[i].y, std::move(enc[i]));
    }
    if (gas_) gas_->rebase(cx0 * kChunkSize, cy0 * kChunkSize);
    const int ocx = wcx_, ocy = wcy_;
    wcx_ = cx0;
    wcy_ = cy0;
    std::vector<ChunkCoord> fresh;
    for (int cy = cy0; cy < cy0 + params_.chunksH; ++cy)
        for (int cx = cx0; cx < cx0 + params_.chunksW; ++cx)
            if (!world_.chunk(cx, cy)) fresh.push_back({cx, cy});
    {
        // cells first, in parallel (pure per chunk), into chunks made up front
        std::vector<Chunk*> made;
        for (const ChunkCoord& c : fresh) made.push_back(world_.createChunk(c.x, c.y));
        parallel(static_cast<int>(fresh.size()), [&](int i) {
            auto it = store_.find(chunkKey(fresh[i].x, fresh[i].y));
            if (it == store_.end()) gen_.generate(*made[i]);
            else decodeCells(it->second.cells, *made[i]);
        });
        for (const ChunkCoord& c : fresh) wakeChunk(c.x, c.y, true);
    }
    // The old window's edge was a wall; wake the seams so things flow across them now.
    for (const ChunkCoord& c : fresh) {
        const int ox = c.x * kChunkSize, oy = c.y * kChunkSize;
        world_.wakeRect(ox - 1, oy - 1, ox + kChunkSize, oy);
        world_.wakeRect(ox - 1, oy + kChunkMask, ox + kChunkSize, oy + kChunkSize);
        world_.wakeRect(ox - 1, oy - 1, ox, oy + kChunkSize);
        world_.wakeRect(ox + kChunkMask, oy - 1, ox + kChunkSize, oy + kChunkSize);
    }
    (void)ocx;
    (void)ocy;
    if (bodies_) bodies_->restampAll();
    stats_.storeChunks = store_.size();
    ++stats_.shifts;
    stats_.lastShiftMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// --------------------------------------------------------------------------
// save files

bool Streamer::saveToBytes(std::vector<std::uint8_t>& out, const SaveExtras& extras) {
    ByteWriter w;
    w.raw(kMagic, sizeof kMagic);
    w.u32(kSaveVersion);
    w.u64(gen_.seed());
    w.u64(sim_ ? sim_->tickCount() : 0);
    w.u64(gas_ ? gas_->tickCount() : 0);
    w.u64(particles_ ? particles_->tickCount() : 0);
    w.u64(bodies_ ? bodies_->tickCount() : 0);
    w.i32(wcx_); w.i32(wcy_); w.i32(params_.chunksW); w.i32(params_.chunksH);
    w.u32(static_cast<std::uint32_t>(mats_.size()));
    for (const Material& m : mats_.all()) w.str(m.name);
    const int ns = gas_ ? gas_->speciesCount() : 0;
    w.u32(static_cast<std::uint32_t>(ns));
    for (int k = 0; k < ns; ++k) w.str(mats_[gas_->species(k)].name);
    w.f64(gas_ ? gas_->lostHeat() : 0.0);
    w.f64(gas_ ? gas_->sourceHeat() : 0.0);
    w.f64(gas_ ? gas_->latentHeat() : 0.0);
    w.i64(generatedMass_);

    // player + inventory
    w.u8(extras.hasPlayer ? 1 : 0);
    const PlayerState& ps = extras.player;
    w.f32(ps.x); w.f32(ps.y); w.f32(ps.vx); w.f32(ps.vy); w.f32(ps.spawnX); w.f32(ps.spawnY); w.f32(ps.health);
    w.u64(ps.tick);
    w.i32(ps.airTicks); w.i32(ps.sinceHurt); w.i32(ps.facing);
    w.u8(ps.grounded); w.u8(ps.stuck); w.u8(ps.ignorePowder); w.u8(ps.wasJump); w.u8(ps.gliding); w.u8(ps.rope);
    w.f32(ps.ropeX); w.f32(ps.ropeY); w.f32(ps.ropeLen);
    w.u8(extras.creative ? 1 : 0);
    w.i64(extras.capacity);
    w.i32(extras.charges);
    w.u32(static_cast<std::uint32_t>(extras.inventory.size()));
    for (const auto& [m, n] : extras.inventory) { w.u16(m); w.i64(n); }

    // live: the bodies leave the grid while their chunks are encoded
    if (bodies_) bodies_->unstampAll();
    std::vector<BodyState> liveBodies;
    if (bodies_) bodies_->snapshot(liveBodies);
    const auto& live = world_.chunks();
    w.u32(static_cast<std::uint32_t>(live.size() + store_.size()));
    auto writeChunk = [&](int cx, int cy, const StoredChunk& sc) {
        w.i32(cx); w.i32(cy);
        w.i32(sc.next.x0); w.i32(sc.next.y0); w.i32(sc.next.x1); w.i32(sc.next.y1);
        w.u8(sc.heatNext ? 1 : 0);
        w.blob(sc.cells);
        w.blob(sc.gas);
        w.u32(static_cast<std::uint32_t>(sc.particles.size()));
        for (const Particle& p : sc.particles) writeParticle(w, p);
        w.u32(static_cast<std::uint32_t>(sc.bodies.size()));
        for (const BodyState& b : sc.bodies) writeBody(w, b);
    };
    for (const Chunk* c : live) writeChunk(c->coord.x, c->coord.y, encodeLive(*c, false));
    for (const auto& [key, sc] : store_)
        writeChunk(static_cast<std::int32_t>(key >> 32), static_cast<std::int32_t>(key & 0xFFFFFFFFu), sc);
    // the awake window's particles and bodies
    const std::vector<Particle> noParticles;
    const auto& lp = particles_ ? particles_->particles() : noParticles;
    w.u32(static_cast<std::uint32_t>(lp.size()));
    for (const Particle& p : lp) writeParticle(w, p);
    w.u32(static_cast<std::uint32_t>(liveBodies.size()));
    for (const BodyState& b : liveBodies) writeBody(w, b);
    if (bodies_) bodies_->restampAll();
    out = std::move(w.buf);
    return true;
}

bool Streamer::save(const std::string& path, const SaveExtras& extras, std::string* error) {
    std::vector<std::uint8_t> bytes;
    saveToBytes(bytes, extras);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (error) *error = "cannot write " + tmp; return false; }
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!f) { if (error) *error = "write failed: " + tmp; return false; }
    }
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { if (error) *error = "cannot replace " + path; return false; }
    return true;
}

bool Streamer::load(const std::string& path, SaveExtras& extras, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (error) *error = "cannot open " + path; return false; }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return loadFromBytes(bytes, extras, error);
}

bool Streamer::loadFromBytes(const std::vector<std::uint8_t>& in, SaveExtras& extras, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    ByteReader r(in.data(), in.size());
    char magic[8];
    r.raw(magic, sizeof magic);
    if (!r.ok() || std::memcmp(magic, kMagic, sizeof magic) != 0) return fail("not a notita save");
    const std::uint32_t version = r.u32();
    if (version == 0 || version > kSaveVersion) return fail("save from a newer version");
    const std::uint64_t seed = r.u64();
    const std::uint64_t simTick = r.u64(), gasTick = r.u64(), partTick = r.u64(), bodyTick = r.u64();
    const int wcx = r.i32(), wcy = r.i32(), ww = r.i32(), wh = r.i32();
    if (ww != params_.chunksW || wh != params_.chunksH) return fail("save has a different window size");
    if (seed != gen_.seed()) return fail("save was made with another world seed");
    const std::uint32_t nm = r.u32();
    if (!r.ok() || nm > 65536) return fail("bad material table");
    std::vector<MaterialId> remap(nm, kEmpty);
    std::string missing;
    for (std::uint32_t i = 0; i < nm; ++i) {
        const std::string name = r.str();
        remap[i] = i == 0 ? kEmpty : mats_.idOf(name);
        if (i != 0 && remap[i] == kEmpty) missing += (missing.empty() ? "" : ", ") + name;
    }
    const std::uint32_t ns = r.u32();
    std::vector<int> speciesRemap(ns, -1);
    for (std::uint32_t k = 0; k < ns; ++k) {
        const std::string name = r.str();
        speciesRemap[k] = gas_ ? gas_->speciesOf(mats_.idOf(name)) : -1;
    }
    const double lost = r.f64(), source = r.f64(), latent = r.f64();
    const long long generated = r.i64();

    extras = SaveExtras{};
    extras.hasPlayer = r.u8() != 0;
    PlayerState& ps = extras.player;
    ps.x = r.f32(); ps.y = r.f32(); ps.vx = r.f32(); ps.vy = r.f32(); ps.spawnX = r.f32(); ps.spawnY = r.f32(); ps.health = r.f32();
    ps.tick = r.u64();
    ps.airTicks = r.i32(); ps.sinceHurt = r.i32(); ps.facing = r.i32();
    ps.grounded = r.u8(); ps.stuck = r.u8(); ps.ignorePowder = r.u8(); ps.wasJump = r.u8(); ps.gliding = r.u8(); ps.rope = r.u8();
    ps.ropeX = r.f32(); ps.ropeY = r.f32(); ps.ropeLen = r.f32();
    extras.creative = r.u8() != 0;
    extras.capacity = r.i64();
    extras.charges = r.i32();
    const std::uint32_t ninv = r.u32();
    if (!r.ok() || ninv > 65536) return fail("bad inventory");
    for (std::uint32_t i = 0; i < ninv; ++i) {
        const std::uint16_t m = r.u16();
        const long long n = r.i64();
        const MaterialId cur = m < remap.size() ? remap[m] : kEmpty;
        if (cur != kEmpty) extras.inventory.emplace_back(cur, n);
    }

    // chunks: decoded (remapped) and re-encoded with the current ids, so the store is uniform
    const std::uint32_t nc = r.u32();
    if (!r.ok()) return fail("truncated save");
    std::map<std::uint64_t, StoredChunk> store;
    long long mass = 0;
    std::int64_t gasMass = 0;
    Chunk scratch;
    const int curSpecies = gas_ ? gas_->speciesCount() : 0;
    for (std::uint32_t i = 0; i < nc; ++i) {
        const int cx = r.i32(), cy = r.i32();
        StoredChunk sc;
        sc.next.x0 = r.i32(); sc.next.y0 = r.i32(); sc.next.x1 = r.i32(); sc.next.y1 = r.i32();
        sc.heatNext = r.u8() != 0;
        const std::vector<std::uint8_t> cells = r.blob();
        const std::vector<std::uint8_t> gas = r.blob();
        if (!r.ok()) return fail("truncated chunk");
        scratch.coord = {cx, cy};
        if (!decodeCells(cells, scratch, &remap)) return fail("corrupt chunk cells");
        sc.cells = encodeCells(scratch);
        sc.mass = cellsMass(scratch, mats_);
        if (!gas.empty() && curSpecies > 0) {
            GasGrid::ChunkCells g;
            if (!decodeGas(gas, g, curSpecies, &speciesRemap)) return fail("corrupt chunk gas");
            sc.gasMass = g.massFixed();
            sc.gas = encodeGas(g);
        }
        const std::uint32_t np = r.u32();
        if (!r.ok() || np > (1u << 24)) return fail("bad particle count");
        for (std::uint32_t q = 0; q < np; ++q) {
            sc.particles.push_back(readParticle(r, remap));
            sc.mass += mats_[sc.particles.back().cell.material].mass;
        }
        const std::uint32_t nb = r.u32();
        if (!r.ok() || nb > 4096) return fail("bad body count");
        for (std::uint32_t q = 0; q < nb; ++q) {
            sc.bodies.push_back(readBody(r, remap));
            sc.mass += bodyMass(sc.bodies.back(), mats_);
        }
        if (!r.ok()) return fail("truncated chunk");
        mass += sc.mass;
        gasMass += sc.gasMass;
        store[chunkKey(cx, cy)] = std::move(sc);
    }
    std::vector<Particle> liveParticles;
    const std::uint32_t np = r.u32();
    if (!r.ok() || np > (1u << 24)) return fail("bad particle count");
    for (std::uint32_t q = 0; q < np; ++q) liveParticles.push_back(readParticle(r, remap));
    std::vector<BodyState> liveBodies;
    const std::uint32_t nb = r.u32();
    if (!r.ok() || nb > 4096) return fail("bad body count");
    for (std::uint32_t q = 0; q < nb; ++q) liveBodies.push_back(readBody(r, remap));
    if (!r.ok()) return fail("truncated save");

    // replace the world
    reset(wcx, wcy);           // empty window, grid at its origin, store forgotten
    std::vector<ChunkCoord> all;
    for (const Chunk* c : world_.chunks()) all.push_back(c->coord);
    for (const ChunkCoord& c : all) world_.removeChunk(c.x, c.y);
    store_ = std::move(store);
    storedMass_ = mass;
    storedGas_ = gasMass;
    for (int cy = wcy; cy < wcy + params_.chunksH; ++cy)
        for (int cx = wcx; cx < wcx + params_.chunksW; ++cx) wakeChunk(cx, cy, false);
    if (particles_ && !liveParticles.empty()) particles_->add(liveParticles.data(), liveParticles.size());
    if (bodies_) {
        for (const BodyState& b : liveBodies) bodies_->restore(b);
        bodies_->restampAll();
    }
    if (sim_) sim_->setTickCount(simTick);
    if (gas_) {
        gas_->setTickCount(gasTick);
        gas_->setHeatTotals(lost, source, latent);
    }
    if (particles_) particles_->setTickCount(partTick);
    generatedMass_ = generated;
    if (bodies_) bodies_->setTickCount(bodyTick);
    stats_.storeChunks = store_.size();
    stats_.storeBytes = 0;
    for (const auto& [k, sc] : store_) stats_.storeBytes += sc.bytes();
    if (error) *error = missing.empty() ? "" : "materials missing from this data (loaded as air): " + missing;
    return true;
}

} // namespace notita
