#pragma once
// Fixed-step cellular automaton. Each tick runs 4 checkerboard passes over
// the chunks: (even x, even y), (odd, even), (even, odd), (odd, odd). Chunks
// in one pass are two chunks apart and a chunk update reaches at most
// kMaxReach pixels outside itself, so a pass runs fully in parallel with no
// locks on cells. Each chunk gets its own RNG stream seeded from
// (seed, tick, chunk), so the result is identical for any thread count.
#include "Material.h"
#include "ThreadPool.h"
#include "World.h"

#include <cstdint>
#include <vector>

namespace notita {

struct SimStats {
    double        tickMs = 0.0;      // wall time of the last tick
    int           activeChunks = 0;  // chunks with a non-empty dirty rect
    int           totalChunks = 0;
    std::uint64_t cellsProcessed = 0; // moving cells visited last tick
    std::uint64_t tick = 0;
};

struct SimParams {
    int gravity = 48;      // 1/256 px per tick^2 (about 0.19 px/tick^2)
};

class Simulator {
public:
    Simulator(World& world, const MaterialRegistry& materials, ThreadPool* pool, std::uint64_t seed);

    void tick();
    std::uint64_t tickCount() const { return tick_; }
    std::uint64_t seed() const { return seed_; }
    const SimStats& stats() const { return stats_; }
    SimParams& params() { return params_; }

private:
    void updateChunk(Chunk& chunk, int pass);

    World& world_;
    const MaterialRegistry& mats_;
    ThreadPool* pool_;
    std::uint64_t seed_;
    std::uint64_t tick_ = 0;
    SimStats stats_;
    SimParams params_;
    std::vector<Chunk*> jobs_;
};

} // namespace notita
