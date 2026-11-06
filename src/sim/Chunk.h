#pragma once
// A 64x64 block of cells. Chunks are the unit of threading (checkerboard
// passes), sleeping (dirty rects) and, later, streaming and saving.
#include "Cell.h"
#include "GasExchange.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace notita {

constexpr int kChunkShift = 6;
constexpr int kChunkSize = 1 << kChunkShift; // 64
constexpr int kChunkMask = kChunkSize - 1;
constexpr int kChunkCells = kChunkSize * kChunkSize;
// How far (in pixels) a chunk update may read or write outside its own chunk.
// Same-pass chunks are two chunks apart, so anything below kChunkSize / 2 can
// never touch what another thread touches in the same pass.
constexpr int kMaxReach = kChunkSize / 2;

struct ChunkCoord {
    int x = 0, y = 0;
    bool operator==(const ChunkCoord&) const = default;
};

inline std::uint64_t chunkKey(int cx, int cy) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) |
           static_cast<std::uint32_t>(cy);
}

// Inclusive rect in chunk-local coordinates. Empty when x0 > x1.
struct DirtyRect {
    int x0 = kChunkSize, y0 = kChunkSize, x1 = -1, y1 = -1;
    bool empty() const { return x0 > x1 || y0 > y1; }
    int area() const { return empty() ? 0 : (x1 - x0 + 1) * (y1 - y0 + 1); }
};

// Dirty rect that several threads may grow at once (neighbours of a chunk can
// mark cells in it during the same pass). Union is order-independent, so it
// stays deterministic.
class AtomicDirtyRect {
public:
    void include(int x0, int y0, int x1, int y1) {
        fetchMin(x0_, x0); fetchMin(y0_, y0);
        fetchMax(x1_, x1); fetchMax(y1_, y1);
    }
    DirtyRect takeAndReset() {
        DirtyRect r{x0_.exchange(kChunkSize, std::memory_order_relaxed),
                    y0_.exchange(kChunkSize, std::memory_order_relaxed),
                    x1_.exchange(-1, std::memory_order_relaxed),
                    y1_.exchange(-1, std::memory_order_relaxed)};
        return r;
    }
    DirtyRect peek() const {
        return {x0_.load(std::memory_order_relaxed), y0_.load(std::memory_order_relaxed),
                x1_.load(std::memory_order_relaxed), y1_.load(std::memory_order_relaxed)};
    }

private:
    static void fetchMin(std::atomic<int>& a, int v) {
        int cur = a.load(std::memory_order_relaxed);
        while (v < cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }
    static void fetchMax(std::atomic<int>& a, int v) {
        int cur = a.load(std::memory_order_relaxed);
        while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }
    std::atomic<int> x0_{kChunkSize}, y0_{kChunkSize}, x1_{-1}, y1_{-1};
};

struct Chunk {
    ChunkCoord coord;
    Cell cells[kChunkCells];
    DirtyRect rect;           // what this tick processes (frozen at tick start)
    AtomicDirtyRect nextRect; // what the next tick will process
    std::uint32_t cellsProcessed = 0; // stats for the last tick
    std::uint32_t events = 0;         // state changes / burns / reactions last tick

    // Heat runs over the whole chunk when the chunk moves (rect not empty)
    // or when heatNext was raised: by itself (temperatures still changing,
    // something burning) or by a neighbour whose edge is warmer or colder.
    bool heatActive = false;              // frozen at tick start
    std::atomic<bool> heatNext{false};
    std::vector<GasEmission> gasOut;      // gas handed to the GasExchange this tick
    double ambientEnergy = 0.0;           // energy air cells lost to the ambient this tick

    Chunk() = default;
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;

    Cell& at(int lx, int ly) { return cells[(ly << kChunkShift) | lx]; }
    const Cell& at(int lx, int ly) const { return cells[(ly << kChunkShift) | lx]; }
    int originX() const { return coord.x * kChunkSize; }
    int originY() const { return coord.y * kChunkSize; }
    bool active() const { return !rect.empty(); }
    bool busy() const { return heatActive || active(); }
};

} // namespace notita
