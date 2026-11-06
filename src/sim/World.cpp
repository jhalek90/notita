#include "World.h"

#include "Rng.h"

#include <algorithm>
#include <cstring>

namespace notita {

World::World(int minCx, int minCy, int chunksW, int chunksH) {
    for (int cy = minCy; cy < minCy + chunksH; ++cy)
        for (int cx = minCx; cx < minCx + chunksW; ++cx) {
            auto c = std::make_unique<Chunk>();
            c->coord = {cx, cy};
            map_[chunkKey(cx, cy)] = std::move(c);
        }
    rebuildList();
}

Chunk* World::createChunk(int cx, int cy) {
    if (Chunk* c = chunk(cx, cy)) return c;
    auto c = std::make_unique<Chunk>();
    c->coord = {cx, cy};
    Chunk* raw = c.get();
    map_[chunkKey(cx, cy)] = std::move(c);
    rebuildList();
    return raw;
}

void World::removeChunk(int cx, int cy) {
    map_.erase(chunkKey(cx, cy));
    rebuildList();
}

void World::rebuildList() {
    list_.clear();
    for (auto& [k, c] : map_) list_.push_back(c.get());
    std::sort(list_.begin(), list_.end(), [](const Chunk* a, const Chunk* b) {
        return a->coord.y != b->coord.y ? a->coord.y < b->coord.y : a->coord.x < b->coord.x;
    });
    if (list_.empty()) { minCx_ = minCy_ = 0; maxCx_ = maxCy_ = -1; return; }
    minCx_ = maxCx_ = list_.front()->coord.x;
    minCy_ = list_.front()->coord.y;
    maxCy_ = list_.back()->coord.y;
    for (const Chunk* c : list_) {
        minCx_ = std::min(minCx_, c->coord.x);
        maxCx_ = std::max(maxCx_, c->coord.x);
    }
}

bool World::setCell(int x, int y, const Cell& c) {
    Cell* dst = cell(x, y);
    if (!dst) return false;
    *dst = c;
    wake(x, y);
    return true;
}

bool World::place(int x, int y, MaterialId m, std::uint8_t shade, float temperature) {
    Cell c;
    c.material = m;
    c.shade = shade;
    c.temperature = temperature;
    return setCell(x, y, c);
}

bool World::addHeat(int x, int y, float degrees) {
    Cell* c = cell(x, y);
    if (!c) return false;
    c->temperature = std::clamp(c->temperature + degrees, kMinTemp, kMaxTemp);
    wake(x, y);
    return true;
}

void World::wake(int x, int y) { wakeRect(x - 1, y - 1, x + 1, y + 1); }

void World::wakeRect(int x0, int y0, int x1, int y1) {
    if (x0 > x1 || y0 > y1) return;
    const int cx0 = x0 >> kChunkShift, cx1 = x1 >> kChunkShift;
    const int cy0 = y0 >> kChunkShift, cy1 = y1 >> kChunkShift;
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cx = cx0; cx <= cx1; ++cx) {
            Chunk* c = chunk(cx, cy);
            if (!c) continue;
            const int ox = cx * kChunkSize, oy = cy * kChunkSize;
            c->nextRect.include(std::max(x0 - ox, 0), std::max(y0 - oy, 0),
                                std::min(x1 - ox, kChunkMask), std::min(y1 - oy, kChunkMask));
            c->heatNext.store(true, std::memory_order_relaxed);
        }
}

void World::wakeAll() {
    for (Chunk* c : list_) {
        c->nextRect.include(0, 0, kChunkMask, kChunkMask);
        c->heatNext.store(true, std::memory_order_relaxed);
    }
}

void World::clear() {
    for (Chunk* c : list_) {
        std::fill(std::begin(c->cells), std::end(c->cells), Cell{});
        c->nextRect.takeAndReset();
        c->rect = DirtyRect{};
        c->heatNext.store(false, std::memory_order_relaxed);
        c->heatActive = false;
        c->gasOut.clear();
        c->particlesOut.clear();
    }
}

int World::beginTick() {
    int active = 0;
    for (Chunk* c : list_) {
        c->rect = c->nextRect.takeAndReset();
        c->heatActive = c->heatNext.exchange(false, std::memory_order_relaxed);
        c->cellsProcessed = 0;
        c->events = 0;
        c->ambientEnergy = 0.0;
        c->splashIn = c->splashOut = 0.0;
        if (!c->rect.empty()) ++active;
    }
    return active;
}

std::uint64_t World::hash() const {
    std::uint64_t h = 0x84222325CBF29CE4ull;
    for (const Chunk* c : list_) {
        h = Rng::hash(h, chunkKey(c->coord.x, c->coord.y));
        for (const Cell& cell : c->cells) {
            const std::uint64_t v = static_cast<std::uint64_t>(cell.material) |
                                    (static_cast<std::uint64_t>(cell.shade) << 16) |
                                    (static_cast<std::uint64_t>(static_cast<std::uint16_t>(cell.vx)) << 24) |
                                    (static_cast<std::uint64_t>(static_cast<std::uint16_t>(cell.vy)) << 40);
            h = (h ^ v) * 0x100000001B3ull;
            std::uint32_t tbits;
            static_assert(sizeof(tbits) == sizeof(cell.temperature));
            std::memcpy(&tbits, &cell.temperature, sizeof tbits);
            const std::uint64_t w = tbits | (static_cast<std::uint64_t>(cell.flags) << 32) |
                                    (static_cast<std::uint64_t>(cell.life) << 40);
            h = (h ^ w) * 0x100000001B3ull;
        }
    }
    return h;
}

long long World::totalMass(const MaterialRegistry& mats) const {
    long long n = 0;
    for (const Chunk* c : list_)
        for (const Cell& cell : c->cells)
            if (!(cell.flags & kFlagBody)) n += mats[cell.material].mass; // body pixels are counted by the BodySystem
    return n;
}

double World::thermalEnergy(const MaterialRegistry& mats) const {
    double e = 0;
    for (const Chunk* c : list_)
        for (const Cell& cell : c->cells) e += static_cast<double>(mats[cell.material].heatCapacity) * cell.temperature;
    return e;
}

std::size_t World::count(MaterialId m) const {
    std::size_t n = 0;
    for (const Chunk* c : list_)
        for (const Cell& cell : c->cells) n += cell.material == m;
    return n;
}

} // namespace notita
