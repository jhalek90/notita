#pragma once
// Phase 3: the gas grid. An Eulerian velocity + pressure grid at one cell
// per 4x4 pixels carries every gas the sim hands over (GasExchange), plus
// smoke and the heat of the air. Split in two:
//
//   GasSolver  - the pure grid math, run once per tick: buoyancy, vorticity
//                confinement, semi-Lagrangian velocity advection, a Jacobi
//                pressure solve + projection, then mass-exact advection and
//                diffusion of gas mass and heat. CpuGasSolver below is the
//                reference (tests, benchmark, fallback); the app has an SDL3
//                GPU compute version with the same kernels (src/app).
//   GasGrid    - the coupling with the pixels, on the CPU: obstacles from the
//                world, taking gas in (emissions and stray gas pixels),
//                trading heat with air pixels, condensing gas back into
//                pixels, burning flammable gas, explosions and gas pushing
//                liquids and powders.
//
// Conservation: gas mass is stored as fixed-point integers (kGasMassOne per
// Material::mass unit) and every transport step moves whole integer fluxes
// from one cell to its neighbour, so world mass + grid mass stays exactly
// constant. Heat is energy above the ambient (heat_capacity * degrees, the
// same units as the pixels) and moves by the same fluxes, so it is conserved
// up to float rounding; the only sources and sinks are burning, explosions,
// latent heat on condensing and the optional loss to the ambient (counted).
//
// Velocity is in grid cells per tick (1 cell = 4 px), +y is down.
#include "GasExchange.h"
#include "Material.h"
#include "Rng.h"
#include "ThreadPool.h"
#include "World.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace notita {

class ParticleSystem;

constexpr int kGasCell = 4;                 // pixels per grid cell side
constexpr int kGasCellPx = kGasCell * kGasCell;
constexpr int kMaxGasSpecies = 8;
constexpr std::int32_t kGasMassOne = 1 << 12; // fixed-point units per Material::mass unit

// Per-tick constants the solver needs (same layout idea as the GPU uniform block).
struct GasSolveParams {
    int   w = 0, h = 0, species = 0;
    int   pressureIters = 40;
    float buoyancyHeat = 0.0008f;    // upward accel per degree above ambient (cells/tick^2)
    float buoyancyDensity = 0.012f;  // per unit of concentration * relative lightness
    float vorticity = 0.12f;
    float damping = 0.992f;          // velocity kept per tick
    float maxSpeed = 1.8f;           // cells per tick
    float diffusion = 0.04f;         // fraction of the mass difference traded per face per tick (< 0.25)
    float airCap = 0.25f;            // heat capacity of one air pixel
    float light[kMaxGasSpecies]{};   // (air density - gas density) / air density, clamped to -4..1
    float cap[kMaxGasSpecies]{};     // heat capacity per kGasMassOne
};

// The grid state. Cell arrays are w*h, row-major; u lives on vertical faces
// ((w+1)*h, u[j*(w+1)+i] is the face left of cell i), v on horizontal faces
// (w*(h+1), v[j*w+i] is the face above cell (i, j)).
struct GasFields {
    int w = 0, h = 0, species = 0;
    std::vector<float> open;          // open pixels / 16 (0 = obstacle)
    std::vector<float> u, v;          // face velocities
    std::vector<float> p;             // pressure (kept between ticks as a warm start)
    std::vector<float> src;           // expansion source this tick (explosions, burning, crowding)
    std::vector<std::int32_t> mass;   // species * w*h, fixed point
    std::vector<float> heat;          // energy above ambient per cell

    void resize(int w, int h, int species);
    std::size_t cells() const { return static_cast<std::size_t>(w) * h; }
    std::int32_t& m(int k, int i) { return mass[static_cast<std::size_t>(k) * cells() + i]; }
    std::int32_t m(int k, int i) const { return mass[static_cast<std::size_t>(k) * cells() + i]; }
    // heat capacity of a cell's gas: its open air plus the gas mass in it
    float capacity(int i, const GasSolveParams& sp) const;
    float speedAt(int i, int j) const; // cell-centred |velocity|
    float uc(int i, int j) const { return 0.5f * (u[j * (w + 1) + i] + u[j * (w + 1) + i + 1]); }
    float vc(int i, int j) const { return 0.5f * (v[j * w + i] + v[(j + 1) * w + i]); }
};

class GasSolver {
public:
    virtual ~GasSolver() = default;
    // One tick of grid math on f (open and src are inputs; u, v, p, mass, heat are updated).
    virtual void solve(GasFields& f, const GasSolveParams& sp) = 0;
    virtual const char* name() const = 0;
};

// The reference implementation. Every kernel is a pure function of the
// previous buffers (double-buffered), so splitting rows over threads gives
// bit-identical results.
class CpuGasSolver : public GasSolver {
public:
    explicit CpuGasSolver(ThreadPool* pool = nullptr) : pool_(pool) {}
    void solve(GasFields& f, const GasSolveParams& sp) override;
    const char* name() const override { return "cpu"; }

private:
    void rows(int count, const std::function<void(int)>& fn);
    ThreadPool* pool_;
    std::vector<float> curl_, u2_, v2_, p2_, div_, scale_, heat2_;
    std::vector<std::int32_t> mass2_;
};

// Shared flux formulas. Both sides of a face call the same function with the
// same inputs, so what leaves one cell is exactly what enters the other.
inline std::int32_t gasAdvectFlux(std::int32_t donorMass, float speed, float donorScale) {
    return static_cast<std::int32_t>(static_cast<float>(donorMass) * (speed * donorScale));
}
inline std::int32_t gasDiffuseFlux(std::int32_t a, std::int32_t b, float d) {
    // a -> b; antisymmetric by construction (truncation towards zero)
    return static_cast<std::int32_t>(static_cast<float>(a - b) * d);
}

struct GasGridParams {
    bool  enabled = true;
    float airExchange = 0.2f;     // fraction per tick air pixels and their grid cell move towards each other
    float ambientLoss = 0.0015f;  // fraction of grid heat lost to the ambient per tick (0 = sealed)
    float crowding = 0.02f;       // expansion source per unit of concentration difference to the neighbours
    float burnExpansion = 0.004f; // expansion per energy unit released by burning gas
    float burnRate = 0.5f;        // fraction of a burning gas that burns per tick
    float pushThreshold = 0.45f;  // gas speed (cells/tick) above which it moves liquids and powders
    float pocketPressure = 1.2f;  // gas units per open pixel above which a pocket shoves loose pixels away
    int   pocketReach = 32;       // longest line of liquid/powder a pocket can shift by one pixel
    float push = 0.6f;            // chance scale for that push
    bool  absorbGasPixels = true; // gas pixels of a grid species (brushes, old saves) are taken in
};

struct GasGridStats {
    double solveMs = 0, coupleMs = 0;
    double obstaclesMs = 0, pushMs = 0; // parts of coupleMs
    long long absorbed = 0;   // units taken in this tick (emissions + gas pixels)
    long long condensed = 0;  // pixels condensed back out this tick
    long long burned = 0;     // units burnt this tick
    int pushed = 0;           // pixels moved by the gas this tick
    int shoved = 0;           // lines of pixels a dense gas pocket shifted outwards this tick
    float maxSpeed = 0, maxPressure = 0;
};

class GasGrid : public GasExchange {
public:
    GasGrid(World& world, const MaterialRegistry& mats, ThreadPool* pool, std::uint64_t seed);

    // GasExchange: every gas material (up to kMaxGasSpecies) goes to the grid.
    bool takesGas(MaterialId gas) const override { return speciesOf(gas) >= 0; }
    void absorb(const GasEmission* emissions, std::size_t count) override;
    void ambientHeat(double energy) override { lostHeat_ += energy; }

    // Call after Simulator::tick(). Couples with the world, then runs the solver.
    void step();
    // A blast centred at a world pixel: an expansion source, heat and a kick
    // to the loose pixels in the radius.
    void explode(int x, int y, int radius, float power);
    // Removes all gas and motion (the heat of the gas is counted as lost).
    void clear();

    // null = the built-in CPU solver
    void setSolver(GasSolver* solver) { solver_ = solver ? solver : &cpu_; }
    // With a particle system attached, explode() throws loose pixels as momentum particles (phase 4).
    void setParticles(ParticleSystem* particles) { particles_ = particles; }
    float blastSpeed = 6.0f; // px/tick a blast of power 1 gives a pixel at its centre
    GasSolver& solver() { return *solver_; }
    GasGridParams& params() { return params_; }
    GasSolveParams& solveParams() { return sp_; }
    const GasFields& fields() const { return f_; }
    GasFields& fields() { return f_; }
    const GasGridStats& stats() const { return stats_; }

    int speciesCount() const { return static_cast<int>(species_.size()); }
    MaterialId species(int k) const { return species_[k]; }
    int speciesOf(MaterialId m) const { return m < speciesIndex_.size() ? speciesIndex_[m] : -1; }
    int originX() const { return ox_; }
    int originY() const { return oy_; }

    // Totals for conservation checks.
    std::int64_t massFixed() const;                // all species, fixed point
    std::int64_t massFixed(int k) const;
    double heatEnergy() const;                    // sum of heat (energy above ambient)
    double lostHeat() const { return lostHeat_; } // given to the ambient (+) or taken from it (-)
    double sourceHeat() const { return sourceHeat_; } // added by burning and explosions
    double latentHeat() const { return latentHeat_; } // given to pixels by condensing (latent part)
    float temperature(int i) const;               // grid cell temperature

private:
    void buildObstacles();
    void injectPending();
    void moveTrapped();
    void exchangeHeat();
    void burnGas();
    void condense();
    void buildSources();
    void pushPixels();
    void expandPockets();
    Rng cellRng(int i, std::uint64_t salt) const;
    // chunk table over the grid's area: pixel access and waking without hash lookups
    void buildChunkTable();
    Chunk* chunkAt(int x, int y) const {
        const int cx = (x >> kChunkShift) - tcx0_, cy = (y >> kChunkShift) - tcy0_;
        if (cx < 0 || cy < 0 || cx >= tcw_ || cy >= tch_) return nullptr;
        return table_[static_cast<std::size_t>(cy) * tcw_ + cx];
    }
    Cell* px(int x, int y) const {
        Chunk* c = chunkAt(x, y);
        return c ? &c->at(x & kChunkMask, y & kChunkMask) : nullptr;
    }
    void wakePx(int x, int y) const; // like World::wake (3x3, motion + heat)

    World& world_;
    const MaterialRegistry& mats_;
    ThreadPool* pool_;
    std::uint64_t seed_;
    std::uint64_t tick_ = 0;
    int ox_ = 0, oy_ = 0;
    GasFields f_;
    GasSolveParams sp_;
    GasGridParams params_;
    GasGridStats stats_;
    CpuGasSolver cpu_;
    GasSolver* solver_ = &cpu_;

    std::vector<Chunk*> table_;
    int tcx0_ = 0, tcy0_ = 0, tcw_ = 0, tch_ = 0;
    std::vector<MaterialId> species_;
    std::vector<int> speciesIndex_; // by material id, -1 = not a grid gas
    std::vector<GasEmission> pending_;
    std::vector<std::uint8_t> flameIn_; // per cell: a flame or burning pixel is in the block
    std::vector<std::uint8_t> loose_;   // per cell: liquid or powder pixels in the block
    std::vector<float> extraSrc_;       // burning: expansion for the next solve
    std::vector<std::uint8_t> burnt_;   // per cell: gas burnt here last tick (the flame front)
    struct Blast { int x, y, radius; float power; };
    std::vector<Blast> blasts_;         // grid side of explode(), applied after the obstacles are rebuilt
    void applyBlast(const Blast& b);
    double lostHeat_ = 0, sourceHeat_ = 0, latentHeat_ = 0;
    long long absorbedPending_ = 0;
    ParticleSystem* particles_ = nullptr;
};

} // namespace notita
