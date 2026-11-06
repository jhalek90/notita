#pragma once
// Phase 4: momentum particles. Liquids that are hit hard (a fast fall onto a
// pool or the ground, a blast, a rigid body ploughing through) leave the
// cellular automaton as particles, fly with gravity, drag and the gas grid's
// wind, and settle back into the first empty cell they are stopped by. The
// cell gets the particle's velocity back, so the automaton carries on with it.
//
// Conservation: a particle holds the cell that left the grid (its material,
// so its Material::mass, and its temperature). The grid cell it left becomes
// air at the ambient temperature; the air pixel it lands on gives its heat
// (relative to ambient) to the landing pixel. A particle that finds no empty
// cell waits, so world mass + particle mass stays constant. Momentum: the
// cell's velocity leaves with the particle and comes back with it, and a
// splash hands out at most `restitution` of the impact momentum (see
// Simulator). Everything here is deterministic: particles are created in
// chunk order and settled in index order, with per-tick seeded RNG.
#include "Material.h"
#include "Particle.h"
#include "Rng.h"
#include "ThreadPool.h"
#include "World.h"

#include <cstdint>
#include <vector>

namespace notita {

class GasGrid;

struct ParticleParams {
    float gravity = 48.0f / 256.0f; // px per tick^2 (matches SimParams::gravity)
    float drag = 0.01f;             // fraction of velocity lost per tick in air
    float gasDrag = 0.12f;          // per tick, pull towards the gas grid's velocity (0 = ignore the grid)
    float maxSpeed = 14.0f;         // px per tick (stays inside what a cell can carry)
    float settleSpeed = 0.6f;       // slower than this and blocked below: settle
    int   maxAge = 900;             // ticks; older particles settle at the first chance
    int   maxCount = 200000;        // launches beyond this are refused (the pixel stays put)
};

struct ParticleStats {
    int    count = 0;      // alive after the step
    int    spawned = 0;    // added this tick
    int    settled = 0;    // put back into the grid this tick
    int    waiting = 0;    // blocked with no room yet
    double stepMs = 0.0;
};

class ParticleSystem {
public:
    ParticleSystem(World& world, const MaterialRegistry& mats, ThreadPool* pool, std::uint64_t seed);

    // Drag and blast coupling with the gas grid (null = none).
    void setGas(const GasGrid* gas) { gas_ = gas; }

    // Takes the pixel at (x, y) out of the grid (air at ambient stays behind)
    // and launches it with (vx, vy) px/tick. False for air, outside, body pixels or when full.
    bool launch(int x, int y, float vx, float vy, float ambient = kAmbientTemp);
    // Adds particles for cells that were already removed from the grid.
    void add(const Particle& p);
    void add(const Particle* p, std::size_t n);

    // One tick: move everything, then settle what stopped (serial, index order).
    void step();
    void clear() { particles_.clear(); }
    // Phase 6 (streaming): moves the particles inside the pixel rect [x0, x1) x [y0, y1)
    // to out (in index order), so a chunk going to sleep takes its flying pixels along.
    void extractIn(int x0, int y0, int x1, int y1, std::vector<Particle>& out);
    std::uint64_t tickCount() const { return tick_; }
    void setTickCount(std::uint64_t t) { tick_ = t; }

    const std::vector<Particle>& particles() const { return particles_; }
    std::size_t count() const { return particles_.size(); }
    ParticleParams& params() { return params_; }
    const ParticleStats& stats() const { return stats_; }

    long long totalMass() const;               // sum of Material::mass over all particles
    double thermalEnergy() const;              // sum of heat_capacity * temperature
    double momentumX() const;                  // sum of mass * vx
    double momentumY() const;

private:
    bool settle(Particle& p, int cx, int cy);
    void move(Particle& p, const Rng& rng);

    World& world_;
    const MaterialRegistry& mats_;
    ThreadPool* pool_;
    std::uint64_t seed_;
    std::uint64_t tick_ = 0;
    const GasGrid* gas_ = nullptr;
    ParticleParams params_;
    ParticleStats stats_;
    std::vector<Particle> particles_;
    std::vector<std::uint8_t> blocked_;   // per particle after the move: stopped this tick
    int spawned_ = 0;
};

} // namespace notita
