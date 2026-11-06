#pragma once
// The world is a sparse set of 64x64 chunks keyed by chunk coordinate.
// Phase 1 creates a fixed rectangle of chunks; streaming (phase 6) only has
// to call createChunk / removeChunk. Unloaded space acts as a solid wall.
#include "Chunk.h"
#include "Material.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace notita {

class World {
public:
    World() = default;
    // Creates chunksW x chunksH chunks starting at chunk (minCx, minCy).
    World(int minCx, int minCy, int chunksW, int chunksH);

    Chunk* createChunk(int cx, int cy);   // returns the existing one if loaded
    void removeChunk(int cx, int cy);

    Chunk* chunk(int cx, int cy) {
        auto it = map_.find(chunkKey(cx, cy));
        return it == map_.end() ? nullptr : it->second.get();
    }
    const Chunk* chunk(int cx, int cy) const {
        auto it = map_.find(chunkKey(cx, cy));
        return it == map_.end() ? nullptr : it->second.get();
    }
    Chunk* chunkAtPixel(int x, int y) { return chunk(x >> kChunkShift, y >> kChunkShift); }
    const Chunk* chunkAtPixel(int x, int y) const { return chunk(x >> kChunkShift, y >> kChunkShift); }

    // All loaded chunks in a stable order (row-major by coordinate).
    const std::vector<Chunk*>& chunks() const { return list_; }

    Cell* cell(int x, int y) {
        Chunk* c = chunkAtPixel(x, y);
        return c ? &c->at(x & kChunkMask, y & kChunkMask) : nullptr;
    }
    const Cell* cell(int x, int y) const {
        const Chunk* c = chunkAtPixel(x, y);
        return c ? &c->at(x & kChunkMask, y & kChunkMask) : nullptr;
    }
    bool inside(int x, int y) const { return chunkAtPixel(x, y) != nullptr; }
    MaterialId material(int x, int y) const {
        const Cell* c = cell(x, y);
        return c ? c->material : kEmpty;
    }

    // Writes a cell and wakes it and its 8 neighbours. Returns false outside the world.
    bool setCell(int x, int y, const Cell& c);
    bool place(int x, int y, MaterialId m, std::uint8_t shade);

    void wake(int x, int y);                  // 3x3 around (x, y)
    void wakeRect(int x0, int y0, int x1, int y1); // inclusive, world pixels
    void wakeAll();
    void clear();                             // everything back to air, all chunks asleep

    // Bounding box of loaded chunks, in pixels.
    int minX() const { return minCx_ * kChunkSize; }
    int minY() const { return minCy_ * kChunkSize; }
    int widthPx() const { return (maxCx_ - minCx_ + 1) * kChunkSize; }
    int heightPx() const { return (maxCy_ - minCy_ + 1) * kChunkSize; }

    // Freezes nextRect into rect for every chunk. Returns the number of active chunks.
    int beginTick();

    std::uint64_t hash() const;               // of every cell's material, shade and velocity
    std::size_t count(MaterialId m) const;

private:
    void rebuildList();
    std::unordered_map<std::uint64_t, std::unique_ptr<Chunk>> map_;
    std::vector<Chunk*> list_;
    int minCx_ = 0, minCy_ = 0, maxCx_ = -1, maxCy_ = -1;
};

} // namespace notita
