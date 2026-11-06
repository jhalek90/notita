#pragma once
// Electricity: a potential field over connected conductive pixels.
//
// Every tick ElectricSystem::step (after Simulator::tick, before
// BodySystem::step) does:
//   1. finds the fixed-potential pixels in the awake window: sources (volt
//      cells, at their `voltage`), drains (ground rods, 0 V) and thermopiles
//      (voltage per degree above ambient);
//   2. flood-fills the conductive pixels connected to them (4-neighbour;
//      `electric` = conductance per pixel, pairs use the harmonic mean;
//      a pressure plate only conducts while something rests on it or the
//      player stands on it; an open switch is an insulator) into components;
//   3. solves Kirchhoff's current law on every component that holds two
//      different fixed potentials (Jacobi-preconditioned conjugate gradient,
//      warm-started from last tick's potentials, a capped number of
//      iterations per tick) - a component with one potential is uniform and
//      carries no current;
//   4. turns the result into effects through the sim: resistive heating
//      (power g * dV^2 per pixel -> temperature, so wires glow and melt
//      through the normal heat pass), lamps that switch to their lit
//      material (an `emissive` material, so the lighting pass sees them),
//      sparks thrown into the air next to pixels carrying a lot of power
//      (flame-kind cells: they ignite flammables and gas like flames do),
//      electrolysis (two touching cells of a `splits_into` liquid become the
//      two products, mass checked at load) and the current the player
//      feels in liquid (Player::step reads currentAt).
// Lightning strikes queued by strike() run in the next step: a bolt of
// sparks down to the first solid or liquid, heat at the hit and a discharge
// that spreads through the conductors it hits.
//
// Everything is serial or per component in a fixed order, and randomness
// comes from an Rng seeded by (seed, tick), so results don't depend on the
// thread count. The potentials (the solver's warm start) live per chunk and
// travel with the chunk through streaming and saves (exportChunk /
// importChunk); the pending strikes and the tick counter are saved by
// save/load.
#include "Material.h"
#include "ThreadPool.h"
#include "World.h"

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace notita {

class Player;

struct ElectricStrike {
    int x = 0, y = 0;
};

enum class ElectricRole : std::uint8_t {
    None,
    Source,     // fixed at `voltage`
    Drain,      // fixed at 0 V
    Thermo,     // fixed at voltage * (T - ambient), clamped
    Lamp,       // switches to toggles_into when powered
    LampLit,    // switches back to toggles_into when not
    Plate,      // conducts only while loaded
    Switch,     // toggled by the player (toggles_into); open switches have electric = 0
};

const char* electricRoleName(ElectricRole r);

// Per-material electric data, read from materials.txt keys (unknown-key path):
//   electric       conductance per pixel, 0..1 (0 / missing = insulator)
//   electric_role  source | drain | thermopile | lamp | lamp_lit | plate | switch
//   voltage        source: its potential; thermopile: volts per degree above ambient
//   toggles_into   lamp <-> lamp_lit, switch open <-> closed (same mass)
//   splits_into    electrolysis: "a, b" - two touching cells become a and b
//   split_current  current through a pixel needed to split it (default 0.05)
//   electric_heat  share of the power that becomes heat (default 1; lamps turn most of it into light)
struct ElectricProps {
    float conduct = 0.0f;
    float heatShare = 1.0f;
    ElectricRole role = ElectricRole::None;
    float voltage = 0.0f;
    MaterialId toggle = kEmpty;
    MaterialId splitA = kEmpty, splitB = kEmpty;
    float splitCurrent = 0.05f;
    bool fixed() const { return role == ElectricRole::Source || role == ElectricRole::Drain || role == ElectricRole::Thermo; }
};

struct ElectricParams {
    bool  enabled = true;
    int   maxIterations = 64;     // CG iterations per component per tick (warm start carries the rest over)
    int   coarseMin = 256;        // pieces with this many pixels or more also get a coarse-grid correction
    int   coarseIterations = 300; // CG iterations on the coarse (8x8 px aggregate) system
    float tolerance = 1e-4f;      // stop when no pixel would move by more than this share of the voltage span
    float settled = 0.01f;        // effects (heat, lamps, sparks, electrolysis) wait until the solve is this close:
                                  // a half-solved potential has fake steep steps that would melt wires.
                                  // A component that starts a tick this close isn't re-solved at all (small
                                  // changes like one split pixel don't cost a full solve every tick)
    float heatGain = 0.2f;        // degrees * heat_capacity per unit of power
    float maxHeat = 250.0f;       // degrees per tick at most
    float lampPower = 0.02f;      // power through a lamp pixel that lights it (goes out below half)
    float sparkPower = 2.0f;      // power at a pixel next to air that throws sparks
    float sparkChance = 0.03f;    // per tick at sparkPower (scales up to 5x)
    float splitRate = 0.00005f;     // electrolysis chance per tick at split_current (scales with the current)
    float thermoMax = 200.0f;     // volts a thermopile can reach
    // lightning
    int   strikeLength = 600;     // px the bolt travels at most
    float strikeHeat = 2500.0f;   // degrees * capacity at the hit pixel
    int   strikeReach = 900;      // conductive pixels the discharge spreads through
    float strikeNodeHeat = 60.0f; // degrees * capacity at the nearest of them (falls off with distance)
    float strikeCurrent = 3.0f;   // current shown / felt at the nearest of them
    float strikeSplit = 0.04f;    // electrolysis chance at the nearest of them
};

struct ElectricStats {
    int nodes = 0;          // conductive pixels connected to a fixed potential
    int components = 0;
    int solved = 0;         // components that needed the solver
    int unsettled = 0;      // ...of which still converging (their effects wait)
    int iterations = 0;     // CG iterations this tick (all components)
    double residual = 0.0;  // worst remaining correction, volts
    double imbalance = 0.0; // worst net current into a piece's free pixels, as a share of its terminal current
    double power = 0.0;     // total power dissipated this tick
    double heat = 0.0;      // energy (degrees * capacity) added as heat this tick
    int lit = 0;            // lit lamp pixels
    int sparks = 0, splits = 0, toggled = 0, strikes = 0; // this tick
    long long splitsTotal = 0;
    double ms = 0.0;
    double floodMs = 0.0, solveMs = 0.0, effectsMs = 0.0;
};

class ElectricSystem {
public:
    ElectricSystem(World& world, const MaterialRegistry& mats, ThreadPool* pool, std::uint64_t seed);

    void step();
    void setPlayer(const Player* player) { player_ = player; } // pressure plates feel the player

    // A lightning bolt in the next step, from (x, y) downwards.
    void strike(int x, int y) { strikes_.push_back({x, y}); }
    const std::vector<ElectricStrike>& pendingStrikes() const { return strikes_; }
    void setPendingStrikes(std::vector<ElectricStrike> s) { strikes_ = std::move(s); }
    // Flips every switch pixel connected to the one nearest (x, y) within radius. Returns the pixels flipped.
    int toggleSwitch(int x, int y, int radius);

    float potentialAt(int x, int y) const;
    float currentAt(int x, int y) const;   // |current| through the pixel last step
    // A chunk's potentials and currents (kChunkCells each), or null when it has none: for overlays.
    const float* potentials(int cx, int cy) const;
    const float* currents(int cx, int cy) const;
    float maxPotential() const { return maxV_; }

    const ElectricProps& props(MaterialId m) const { return props_[m]; }
    bool conductive(MaterialId m) const { return props_[m].conduct > 0.0f; }
    ElectricParams& params() { return params_; }
    const ElectricStats& stats() const { return stats_; }
    const std::vector<std::string>& errors() const { return errors_; } // rejected material data
    std::uint64_t tickCount() const { return tick_; }
    void setTickCount(std::uint64_t t) { tick_ = t; }

    // Streaming: a chunk's potentials (empty blob = none). exportChunk with clear drops them.
    bool exportChunk(int cx, int cy, std::vector<std::uint8_t>& out, bool clear);
    void importChunk(int cx, int cy, const std::vector<std::uint8_t>& blob);
    void clear();
    std::size_t fieldChunks() const { return fields_.size(); }

private:
    struct Field {
        std::vector<float> v, i;  // v: NaN where the pixel had no potential
        Field() : v(kChunkCells, std::numeric_limits<float>::quiet_NaN()), i(kChunkCells, 0.0f) {}
    };
    using Strike = ElectricStrike;

    void buildGrid();
    bool loaded(int gx, int gy) const;
    bool conductsAt(int gx, int gy) const;
    Cell* gridCell(int gx, int gy) const;
    void flood();
    void solve(int comp, bool parallel);
    void coarseCorrect(int comp);
    void split(Cell& a, Cell& b, const ElectricProps& p);
    void effects();
    void runStrike(const Strike& s);
    void heatCell(int gx, int gy, Cell& c, float energy);
    void wakeAt(int gx, int gy, bool motion);
    Field& fieldAt(int gx, int gy);

    World& world_;
    const MaterialRegistry& mats_;
    ThreadPool* pool_;
    std::uint64_t seed_;
    std::uint64_t tick_ = 0;
    ElectricParams params_;
    ElectricStats stats_;
    const Player* player_ = nullptr;
    std::vector<ElectricProps> props_;
    std::vector<std::string> errors_;
    MaterialId spark_ = kEmpty;
    std::vector<Strike> strikes_;
    std::map<std::uint64_t, Field> fields_; // per chunk key; ordered (deterministic)
    float maxV_ = 0.0f;

    // per-step scratch, over the loaded chunks' bounding box
    int gx0_ = 0, gy0_ = 0, gw_ = 0, gh_ = 0, cw_ = 0, ch_ = 0;
    std::vector<Chunk*> grid_;
    std::vector<std::int32_t> nodeOf_;
    std::vector<std::vector<std::uint16_t>> seeds_, lit_;
    std::vector<std::int32_t> npx_;            // node -> grid pixel
    std::vector<std::int16_t> nx_, ny_;        // node -> grid x, y
    std::vector<std::uint8_t> nfresh_;         // node had no stored potential
    std::vector<MaterialId> nmat_;             // node -> its material at the flood
    std::vector<std::uint8_t> cond_;           // per grid pixel: conducts this step (filled by the parallel scan)
    std::vector<std::uint8_t> serialRole_;     // per material: lamps / electrolysis need the serial effects pass
    std::vector<std::uint8_t> chunkHas_;       // per grid chunk: has nodes this step
    std::vector<Field*> fieldGrid_;            // per grid chunk: its field (when it has nodes)
    std::vector<float> ng_;                    // node conductance
    std::vector<std::int32_t> nnb_;            // 4 per node, -1 = none
    std::vector<float> nge_;                   // 4 per node, edge conductance
    std::vector<std::uint8_t> nfixed_;
    std::vector<double> nv_, nr_, nz_, np_, nq_, nd_;
    std::vector<float> npow_, ncur_;
    std::vector<std::int32_t> compStart_;      // compStart_[k] .. compStart_[k + 1]
    std::vector<int> compIters_;
    std::vector<std::uint8_t> compSettled_;
    std::vector<std::int32_t> nodeComp_;
    std::vector<double> compResidual_, compImbalance_;
    std::vector<std::uint32_t> mark_;
    std::uint32_t markStamp_ = 0;
};

} // namespace notita
