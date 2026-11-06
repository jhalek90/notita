#pragma once
// Fixed-step cellular automaton. Each tick runs 4 checkerboard passes over
// the chunks: (even x, even y), (odd, even), (even, odd), (odd, odd). Chunks
// in one pass are two chunks apart and a chunk update reaches at most
// kMaxReach pixels outside itself, so a pass runs fully in parallel with no
// locks on cells. Each chunk gets its own RNG stream seeded from
// (seed, tick, chunk), so the result is identical for any thread count.
//
// One chunk update does, in order:
//   1. motion over its dirty rect (powders, liquids, gases, flames) and
//      reactions for reactive cells it visits (Reactions.h);
//   2. heat over the whole chunk when it moved or its heat is awake:
//      pairwise conduction with the right and lower neighbour (energy
//      conserving, writes 1 px into the next chunk), state changes
//      (melt/boil/freeze/condense, mass conserving), ignition and burning,
//      and air relaxing towards the ambient temperature.
#include "GasExchange.h"
#include "Material.h"
#include "Reactions.h"
#include "ThreadPool.h"
#include "World.h"

#include <cstdint>
#include <vector>

namespace notita {

class ParticleSystem;

struct SimStats {
    double        tickMs = 0.0;      // wall time of the last tick
    int           activeChunks = 0;  // chunks with a non-empty dirty rect (motion)
    int           heatChunks = 0;    // chunks that ran the heat pass
    int           totalChunks = 0;
    std::uint64_t cellsProcessed = 0; // moving cells visited last tick
    std::uint64_t events = 0;        // state changes, ignitions, burn-outs and reactions last tick
    std::uint64_t tick = 0;
    int    splashed = 0;             // pixels that left the grid as splash particles last tick
    double splashMomentumIn = 0.0;   // |momentum| (mass units * px/tick) of the impacts that splashed
    double splashMomentumOut = 0.0;  // |momentum| handed to the ejecta (<= restitution * in)
};

struct SimParams {
    int   gravity = 48;          // 1/256 px per tick^2 (about 0.19 px/tick^2)
    bool  heat = true;           // heat pass (conduction, state changes, fire)
    bool  reactions = true;      // reaction table lookups
    float conduction = 1.0f;     // scales every conductivity (0..1 is the stable range)
    float ambientTemp = kAmbientTemp;
    float ambientRate = 0.004f;  // fraction per tick that air relaxes towards ambientTemp (0 = sealed)
    // phase 4: splashes (only with a ParticleSystem attached) and liquid mixing
    float splashSpeed = 5.0f;    // px per tick a liquid must hit with to splash
    float restitution = 0.6f;    // share of the impact momentum the ejecta may carry
    float splashChance = 0.7f;   // chance per candidate surface pixel to fly
    float mixRate = 0.1f;        // chance per tick a miscible liquid swaps with a different miscible neighbour
    int   mixSteps = 48;         // swaps a pixel may take part in before it counts as blended (Cell::aux)
    // #477: grains keep their speed between ticks, queue behind moving grains instead of
    // stopping dead, and turn a landing into sideways roll (false = the phase 1 rule)
    bool  smoothPowder = true;
    // #477: resting liquids only flow sideways on a flat surface with momentum or as a
    // lone drop, so a calm pool's top row doesn't flicker (false = the phase 1 rule)
    bool  calmLiquid = true;
    // #480: with a gas exchange that takes flames, a burning pixel hands its flame to the
    // grid as fuel vapour (flame_chance x the flame material's flame_fuel, per tick)
    // instead of throwing flame pixels; a few still fly as embers (the flame's `ember`)
    bool  gridFlames = true;
    float flameFuel = 1.0f;      // scales that fuel
    float emberChance = 0.05f;   // share of the old flame throws that still happen, as embers
};

// Per-material values the tick needs, precomputed from the registry.
struct ThermalProps {
    float invCap = 1.0f, cap = 1.0f;
    float hotPoint = kNoPoint, coldPoint = -kNoPoint;
    MaterialId hotInto = kEmpty, coldInto = kEmpty;
    float hotLatent = 0.0f, coldLatent = 0.0f;   // energy taken / given by the change
    std::uint32_t change16 = 0;                   // change chance, 16-bit
    float ignite = kNoPoint;
    int   burnTime = 0;
    float burnDT = 0.0f, burnMax = 0.0f;
    std::uint32_t flame16 = 0, residue16 = 0;
    MaterialId burnsInto = kEmpty, residue = kEmpty, flame = kEmpty;
    float fuel = 0.0f;         // #480: fuel per burning tick handed to the grid (0 = throw flame pixels)
    MaterialId ember = kEmpty; // #480: what still flies off as pixels with grid flames
    float spawnTemp = kAmbientTemp;
    int   lifeMin = 1, lifeMax = 1;
    bool  events = false;      // has any change / fire behaviour
    bool  reactive = false;
    bool  wakeForReaction = false; // has a temperature-conditioned reaction
    float reactMin = -kNoPoint;
    bool  takesGas = false;    // the GasExchange takes this gas when it is created
};

// Conduction factors for a pair: the first cell's temperature moves by
// a * (T1 - T2), the second's by b * (T1 - T2); a*C1 == b*C2 (energy kept).
struct PairConduct {
    float a = 0.0f, b = 0.0f;
};

class Simulator {
public:
    Simulator(World& world, const MaterialRegistry& materials, ThreadPool* pool, std::uint64_t seed,
              const ReactionTable* reactions = nullptr);

    void tick();
    std::uint64_t tickCount() const { return tick_; }
    void setTickCount(std::uint64_t t) { tick_ = t; } // phase 6: restoring a save
    std::uint64_t seed() const { return seed_; }
    const SimStats& stats() const { return stats_; }
    SimParams& params() { return params_; }

    void setReactions(const ReactionTable* reactions);
    // Phase 3 hook: hand created gases and ambient heat to the gas grid (null = off).
    void setGasExchange(GasExchange* exchange);
    // Phase 4 hook: liquids hit hard leave the grid as particles (null = off, no splashes).
    void setParticles(ParticleSystem* particles) { particles_ = particles; }

    const std::vector<ThermalProps>& thermal() const { return thermal_; }

private:
    void updateChunk(Chunk& chunk, int pass);
    void rebuildTables();

    World& world_;
    const MaterialRegistry& mats_;
    ThreadPool* pool_;
    std::uint64_t seed_;
    std::uint64_t tick_ = 0;
    SimStats stats_;
    SimParams params_;
    std::vector<Chunk*> jobs_;

    const ReactionTable* reactions_ = nullptr;
    GasExchange* gas_ = nullptr;
    ParticleSystem* particles_ = nullptr;
    std::vector<ThermalProps> thermal_;
    std::vector<PairConduct> pairs_;   // n * n
    std::vector<std::uint16_t> reactPairs_; // n * n: reaction index + 1, or 0
    float tablesConduction_ = -1.0f;   // params_.conduction the tables were built for
    bool anyMiscible_ = false;
    std::vector<GasEmission> emissions_;
};

} // namespace notita
