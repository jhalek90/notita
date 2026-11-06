#pragma once
// Phase 4: rigid bodies made of pixels, on Box2D v3.
//
// A connected clump of solid pixels (4-connected) that is not held up becomes
// a rigid body: it falls, tumbles, collides with the pixel world and with
// other bodies, and shatters into fragments on a hard impact. The body owns
// its pixels (material, temperature, shade, in body-local coordinates) and
// stamps them into the grid every tick with kFlagBody set, so the cellular
// automaton sees it as solid: liquids flow around it, powders pile on it,
// heat conducts through it, and a pixel that melts or burns while stamped
// simply leaves the body. Loose pixels the stamp lands on are shoved aside
// as particles (see Particles.h), so nothing is ever overwritten.
//
// What holds a clump up: an anchor material (bedrock) or the world boundary
// anywhere in the clump, or, for a clump that physics already put down
// (Cell::aux == kAuxSettled on its pixels), something under one of its
// pixels: powder, another body, or pinned scene geometry (aux == kAuxPinned,
// never examined itself and never part of a clump, so a block stuck to the
// side of a scene wall falls while one resting on a ledge may tip). What the
// brush places or what a state change creates (aux 0) is live. Clumps are
// looked for in chunks that moved since the last search, every
// checkInterval ticks, so a settled world costs nothing.
//
// Mass: a body's pixels are counted by totalMass(); World::totalMass skips
// cells with kFlagBody, so world + particles + bodies is the conserved sum.
// Box2D's own mass (shape density) only drives the dynamics. The pixel grid
// is the terrain: each tick the solid/powder pixels around every awake body
// are turned into static boxes (row runs merged into rectangles), used for
// one step and thrown away. Units: kPxPerMeter pixels per Box2D metre, one
// tick = 1/60 s.
#include "Material.h"
#include "Particles.h"
#include "Rng.h"
#include "World.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace notita {

class GasGrid;

constexpr std::uint8_t kAuxSettled = 1; // solid pixel put down by physics (re-examined when things move)
constexpr std::uint8_t kAuxPinned = 2;  // solid pixel placed by a scene: never a body

struct BodyPixel {
    std::int16_t lx = 0, ly = 0; // body-local cell (the pixel centre is at lx + 0.5, ly + 0.5)
    Cell cell;
};

struct RigidBody {
    int id = 0;
    std::vector<BodyPixel> pixels;
    std::vector<std::int32_t> stampX, stampY; // where each pixel was written last stamp (INT32_MIN = hidden)
    float x = 0, y = 0;         // body origin in world pixels
    float c = 1, s = 0;         // rotation (cos, sin)
    float vx = 0, vy = 0;       // px per tick
    float w = 0;                // radians per tick
    bool  awake = true;         // moving faster than BodyParams::restSpeed
    int   asleep = 0;           // consecutive ticks at rest
    int   cooldown = 0;         // ticks before it may shatter again
    int   hidden = 0;           // pixels with no room in the grid last stamp
    bool  rebuild = false;      // pixels changed: shapes must be rebuilt before the next step
    long long mass = 0;         // sum of Material::mass over the pixels
    int   submerged = 0;        // pixels below the waterline of the liquid it touches (phase 5 buoyancy)
    bool  floats = false;       // lighter than that liquid: it stays a body instead of freezing into pixels

    // world position of a pixel centre
    void toWorld(const BodyPixel& p, float& wx, float& wy) const {
        const float lx = static_cast<float>(p.lx) + 0.5f, ly = static_cast<float>(p.ly) + 0.5f;
        wx = x + c * lx - s * ly;
        wy = y + s * lx + c * ly;
    }
};

// Phase 6: what a body keeps while its chunk sleeps or sits in a save file.
struct BodyState {
    std::vector<BodyPixel> pixels;
    float x = 0, y = 0, c = 1, s = 0, vx = 0, vy = 0, w = 0;
    int cooldown = 0, asleep = 0;
    bool floats = false;
};

struct BodyParams {
    bool  enabled = true;
    int   checkInterval = 2;     // ticks between clump searches
    int   minPixels = 6;         // smaller clumps stay static pixels
    int   maxPixels = 6000;      // bigger clumps stay static (too many shapes)
    int   maxBodies = 256;
    float shatterSpeed = 5.0f;   // px/tick of velocity change in one step that breaks a body
    int   shatterCooldown = 30;  // ticks a fresh fragment is safe
    int   freezeAfter = 45;      // ticks at rest before a body turns back into plain pixels
    float restSpeed = 0.04f;     // px/tick below which a body counts as resting
    float gasDrag = 0.03f;       // per tick, pull of the gas grid's wind on a body
    float gasPush = 0.8f;        // px/tick^2 per unit of pressure gradient (per px)
    float blastKick = 3.0f;      // px/tick of velocity a blast of power 1 gives a body at its centre
    float pxPerMeter = 32.0f;
    int   subSteps = 4;
    // phase 5: buoyancy. Pixels below the highest liquid pixel touching the
    // body are submerged; they get an upward force of the liquid's density
    // times their area times gravity, at their centroid, plus drag.
    bool  buoyancy = true;
    float liquidDrag = 15.0f;    // per second, on the submerged share of the body's velocity (about half the critical damping of a 4 px raft)
};

struct BodyStats {
    int bodies = 0, awake = 0, pixels = 0;
    int created = 0, frozen = 0, shattered = 0; // this tick
    int displaced = 0;                          // loose pixels shoved out as particles this tick
    int hidden = 0;                             // body pixels with no room in the grid
    int terrainShapes = 0;
    int floating = 0;                           // bodies with submerged pixels
    double stepMs = 0.0, findMs = 0.0, solveMs = 0.0;
};

class BodySystem {
public:
    BodySystem(World& world, const MaterialRegistry& mats, ParticleSystem* particles, std::uint64_t seed);
    ~BodySystem();
    BodySystem(const BodySystem&) = delete;
    BodySystem& operator=(const BodySystem&) = delete;

    void setGas(const GasGrid* gas) { gas_ = gas; }

    // Call after Simulator::tick() (and before the particle step).
    void step();
    // A blast: bodies in the radius get an outward kick.
    void explode(int x, int y, int radius, float power);
    // Cuts the solid clump under (x, y) loose right now, pinned or not (a tool). Returns the body id or 0.
    int releaseClump(int x, int y);
    // Drops every body: pixels are written back as plain pixels.
    void clear();

    // ---- phase 5: what the player touches ----
    // Id of the body whose stamp covers the world pixel (0 = none).
    int bodyAt(int x, int y) const;
    const RigidBody* body(int id) const;
    // Velocity (px/tick) of the body's material at a world point.
    bool velocityAt(int id, float wx, float wy, float& vx, float& vy) const;
    // Impulse at a world point, in water-pixel masses * px/tick (one pixel of
    // density 1000 moving 1 px/tick has 1 unit of momentum).
    bool applyImpulse(int id, float wx, float wy, float ix, float iy);

    // ---- phase 6: streaming and saves ----
    // Takes every body's pixels out of the grid now (the next stamp puts them back).
    void unstampAll();
    // Writes every body back into the grid (after unstampAll and the chunk swap).
    void restampAll();
    // Moves the bodies whose pixel centroid is inside [x0, x1) x [y0, y1) out
    // of the system (call after unstampAll). Ids are not kept.
    void extractIn(int x0, int y0, int x1, int y1, std::vector<BodyState>& out);
    // Every body (for a save), without removing it (call after unstampAll).
    void snapshot(std::vector<BodyState>& out) const;
    int restore(const BodyState& st); // a new body (not stamped until restampAll / the next step)
    std::uint64_t tickCount() const { return tick_; }
    void setTickCount(std::uint64_t t) { tick_ = t; }
    static void centroid(const RigidBody& b, float& cx, float& cy);

    const std::vector<RigidBody>& bodies() const { return bodies_; }
    BodyParams& params() { return params_; }
    const BodyStats& stats() const { return stats_; }
    long long totalMass() const;
    double thermalEnergy() const;

    struct Box { int x0, y0, x1, y1; };
    // Greedy rectangle cover of a mask (w*h bytes, non-zero = set): row runs,
    // merged with the identical run above. Exposed for the tests.
    static void coverBoxes(const std::uint8_t* mask, int w, int h, std::vector<Box>& out);

private:
    struct Impl;
    struct Clump { std::vector<std::int32_t> x, y; bool anchored = false; bool settled = true; bool resting = false; };

    void findClumps();
    bool flood(int sx, int sy, Clump& out, bool ignorePins);
    int  createBody(const Clump& c, float vx, float vy, float w);
    int  createBody(std::vector<BodyPixel>&& pixels, float x, float y, float c, float s, float vx, float vy, float w, int cooldown);
    void buildShapes(RigidBody& b);
    void destroyBody(RigidBody& b);
    void unstamp(RigidBody& b);
    void stamp(RigidBody& b);
    void freeze(RigidBody& b);
    void shatter(RigidBody& b);
    void buildTerrain();
    void applyGas(RigidBody& b);
    void applyLiquid(RigidBody& b);
    void pointVelocity(const RigidBody& b, float wx, float wy, float& vx, float& vy) const;
    void eject(const Cell& cell, float x, float y, float vx, float vy);
    Rng rng(std::uint64_t salt) const;

    World& world_;
    const MaterialRegistry& mats_;
    ParticleSystem* particles_;
    const GasGrid* gas_ = nullptr;
    std::uint64_t seed_;
    std::uint64_t tick_ = 0;
    BodyParams params_;
    BodyStats stats_;
    std::vector<RigidBody> bodies_;
    std::unique_ptr<Impl> impl_;
    std::vector<std::uint8_t> visited_; // clump search scratch over the world's pixel box
    std::vector<std::uint8_t> pending_; // per chunk (world order): moved since the last clump search
    std::vector<std::pair<int, int>> stack_; // flood fill scratch
    int nextId_ = 1;
};

} // namespace notita
