#pragma once
// Phase 5: the player. A 20 px tall capsule that moves through the pixel
// world on its own (it is not stamped into the grid): it collides with solid
// and powder pixels (rigid-body stamps included), steps up small ledges,
// floats by the density of the liquid it is in, is pushed by the gas grid's
// wind and pressure, and takes damage from heat, flames, cold and hard
// landings.
//
// Tools all act through the sim:
//   Dig     - pixels under the brush go into the inventory (material counts);
//   Place   - inventory material goes back out with the brush (into air only);
//   Jet     - a liquid from the inventory is boiled (or a fuel burnt) into its
//             gas, which enters the gas grid at the nozzle with a velocity
//             kick (GasGrid::jet); the recoil pushes the player the other way;
//   Grapple - a rope to the first solid pixel along the aim (moves with a
//             rigid body it hooks, and pulls on it);
//   Blast   - a charge set off through the explosion path (blastAt: gas grid,
//             bodies and the player's own kick), for explosive jumps;
//   Raft    - a block of solid from the inventory, cut loose as a rigid body
//             right away, so it floats if it is lighter than the liquid;
//   Freeze  - a cooling brush: water freezes into frost you can stand on.
//
// Mass: in play mode the inventory holds whole pixels, so world + particles
// + bodies + gas grid + inventory mass stays constant through digging,
// placing, rafts and the jet. Creative mode has every material without
// limit (like the sandbox brushes, it creates and deletes mass).
//
// Order per tick: Simulator::tick -> BodySystem::step -> ParticleSystem::step
// -> GasGrid::step -> Player::step (the player reads this tick's wind and
// body stamps; its jet gas and blasts take effect in the next grid step).
#include "Brush.h"
#include "Material.h"
#include "Rng.h"
#include "World.h"

#include <cstdint>
#include <vector>

namespace notita {

class BodySystem;
class GasGrid;

enum class Tool : std::uint8_t { Dig, Jet, Grapple, Blast, Raft, Freeze, Count };
const char* toolName(Tool t);

class Inventory {
public:
    void reset(std::size_t materials) { count_.assign(materials, 0); }
    bool creative = false;       // every material, no limits, nothing counted
    long long capacity = 8000;   // pixels (play mode)
    int charges = 6;             // blast charges (play mode)

    long long count(MaterialId m) const { return m < count_.size() ? count_[m] : 0; }
    long long total() const;
    bool has(MaterialId m, long long n = 1) const { return creative || count(m) >= n; }
    bool room(long long n = 1) const { return creative || total() + n <= capacity; }
    void add(MaterialId m, long long n = 1);
    bool take(MaterialId m, long long n = 1);
    long long totalMass(const MaterialRegistry& mats) const; // what it holds (0 in creative)
    const std::vector<long long>& counts() const { return count_; }

private:
    std::vector<long long> count_;
};

struct PlayerParams {
    int   height = 20;            // px
    int   halfWidth = 3;          // px each side of the centre column (7 px wide)
    float density = 900.0f;       // floats in water (1000) with the head out, sinks in lamp oil (820)
    float gravity = 48.0f / 256.0f; // px/tick^2, the sim's gravity
    float walkSpeed = 1.1f;       // px/tick
    float groundAccel = 0.25f, airAccel = 0.07f;
    float jumpSpeed = 3.3f;       // px/tick: a jump about 28 px high
    int   coyoteTicks = 5;
    int   stepHeight = 4;         // ledges up to this high are walked up
    float maxSpeed = 12.0f;       // px/tick, any direction
    float swimAccel = 0.30f;      // px/tick^2 up (jump) or down (down) in liquid
    float liquidDrag = 0.05f;     // fraction of velocity lost per tick when fully submerged
    float gasDrag = 0.015f;       // per tick, pull towards the gas grid's wind (still air: falls top out near g / gasDrag = 12.5)
    float gasPush = 0.8f;         // px/tick^2 per unit of pressure gradient per px
    float glideDrag = 0.2f;       // vertical wind pull while gliding (jump held while falling): ~1 px/tick sink
    // health
    float maxHealth = 100.0f;
    float hurtTemp = 70.0f;       // C: hotter cells around the player hurt
    float coldTemp = -30.0f;      // C: colder cells hurt
    float heatDamage = 0.004f;    // hp per tick per degree past hurtTemp (or below coldTemp)
    float flameDamage = 0.5f;     // hp per tick while touching flames or burning pixels
    float landSpeed = 6.5f;       // px/tick: harder landings hurt (a drop of about 120 px)
    float landDamage = 12.0f;     // hp per px/tick above landSpeed
    float regen = 0.05f;          // hp per tick...
    int   regenDelay = 120;       // ...after this many ticks without damage
    // tools
    float reach = 56.0f;          // px from the player's centre
    int   digRate = 24;           // pixels dug per tick at most
    float digHardness = 2.0f;     // dig chance per tick = 1 / (1 + digHardness * hardness)
    int   placeRate = 24;         // pixels placed per tick at most
    int   jetRate = 1;            // propellant pixels per tick
    float jetExhaust = 40.0f;     // px/tick: recoil = jetRate * mass * jetExhaust / player mass
    float jetGasSpeed = 7.0f;     // px/tick given to the grid at the nozzle
    float ropeMax = 160.0f;       // px
    float ropeReel = 1.5f;        // px per tick
    int   blastRadius = 10;
    float blastPower = 1.3f;
    float blastReach = 40.0f;     // px: how far away a charge can be set
    float blastKick = 7.0f;       // px/tick a blast of power 1 gives the player at its centre
    int   raftW = 48, raftH = 6;  // 288 px of timber: enough spare buoyancy in water to carry the player
    float freezeRate = 14.0f;     // degrees per tick at the brush centre
    float freezeFloor = -12.0f;   // C: the freeze tool cools nothing below this
};

struct PlayerInput {
    float move = 0.0f;            // -1 left .. 1 right
    bool  jump = false;           // held: jump, swim up, reel the rope in
    bool  down = false;           // held: swim down, let the rope out
    bool  use = false;            // tool held
    bool  usePressed = false;     // tool went down this tick (grapple, blast, raft)
    bool  place = false;          // place held
    float aimX = 0, aimY = 0;     // world pixels
    Tool  tool = Tool::Dig;
    MaterialId material = kEmpty; // to place, for the jet and rafts
    int   brushRadius = 3;
    BrushShape shape = BrushShape::Circle;
    bool  placePinned = true;     // placed solids are scene geometry (never fall as bodies)
};

struct PlayerStats {
    int dug = 0, placed = 0;      // this tick
    int jetMass = 0;              // gas units the jet put into the grid this tick
    float damage = 0;             // this tick
    long long jetMassTotal = 0;
    int deaths = 0;
    float submerged = 0;          // share of the capsule in liquid
    float windX = 0, windY = 0;   // px/tick, the grid wind around the player
    float hottest = kAmbientTemp; // C, around the player
    bool  blasted = false;        // a charge went off this tick
};

class Player {
public:
    Player(World& world, const MaterialRegistry& mats, std::uint64_t seed);

    void setGas(GasGrid* gas) { gas_ = gas; }
    void setBodies(BodySystem* bodies) { bodies_ = bodies; }

    // Feet at (x, y): the capsule covers rows floor(y) - height .. floor(y) - 1.
    void spawn(float x, float y);
    void setSpawnPoint(float x, float y) { spawnX_ = x; spawnY_ = y; }
    void step(const PlayerInput& in);
    // The player's share of a blast (blastAt calls this).
    void explode(int x, int y, int radius, float power);

    float x() const { return x_; }
    float y() const { return y_; }
    float vx() const { return vx_; }
    float vy() const { return vy_; }
    float centreY() const { return y_ - static_cast<float>(params_.height) * 0.5f; }
    float health() const { return health_; }
    bool  grounded() const { return grounded_; }
    bool  gliding() const { return gliding_; }
    int   facing() const { return facing_; }
    int   groundBody() const { return groundBody_; }
    bool  covers(int wx, int wy) const; // the pixel is inside the capsule
    float massUnits() const;            // in water pixels (Material::mass-like units at density 1000)

    bool  roped() const { return rope_; }
    float ropeX() const { return ropeX_; }
    float ropeY() const { return ropeY_; }
    float ropeLength() const { return ropeLen_; }
    int   ropeBody() const { return ropeBody_; }

    Inventory& inventory() { return inv_; }
    const Inventory& inventory() const { return inv_; }
    PlayerParams& params() { return params_; }
    const PlayerStats& stats() const { return stats_; }
    // The gas a material makes in the jet (boils or burns into), kEmpty if none.
    MaterialId jetGas(MaterialId m) const;
    float hardness(MaterialId m) const { return m < hardness_.size() ? hardness_[m] : 1.0f; }
    bool diggable(const Cell& c) const;

private:
    int  halfWidthAt(int row) const;    // row 0 = top
    bool blocked(int x, int y) const;
    bool collides(int ix, int iy) const;
    void unstick();
    void sense();
    void moveX(float dx);
    void moveY(float dy);
    void hurt(float hp);
    void useTools(const PlayerInput& in);
    void dig(const PlayerInput& in, float tx, float ty);
    void place(const PlayerInput& in, float tx, float ty);
    void jet(const PlayerInput& in);
    void grapple(const PlayerInput& in);
    void rope(const PlayerInput& in);
    void raft(const PlayerInput& in, float tx, float ty);
    void aimPoint(const PlayerInput& in, float reach, float& tx, float& ty) const;
    Rng rng(std::uint64_t salt) const;

    World& world_;
    const MaterialRegistry& mats_;
    GasGrid* gas_ = nullptr;
    BodySystem* bodies_ = nullptr;
    std::uint64_t seed_;
    std::uint64_t tick_ = 0;
    PlayerParams params_;
    PlayerStats stats_;
    Inventory inv_;
    std::vector<float> hardness_;

    float x_ = 0, y_ = 0, vx_ = 0, vy_ = 0;
    float spawnX_ = 0, spawnY_ = 0;
    float health_ = 100.0f;
    bool  grounded_ = false;
    int   airTicks_ = 0;
    int   sinceHurt_ = 0;
    int   facing_ = 1;
    int   groundBody_ = 0;
    bool  stuck_ = false;
    bool  ignorePowder_ = false;
    bool  wasJump_ = false;
    bool  gliding_ = false;
    // environment, sensed each tick
    float submerged_ = 0, liquidDensity_ = 0, liquidViscosity_ = 0;
    // rope
    bool  rope_ = false;
    float ropeX_ = 0, ropeY_ = 0, ropeLen_ = 0;
    int   ropeBody_ = 0;
    float ropeLx_ = 0, ropeLy_ = 0;     // body-local anchor
};

// A blast through the explosion path: the gas grid (pressure, heat, loose
// pixels thrown as particles), rigid bodies and the player all get it.
void blastAt(GasGrid* gas, BodySystem* bodies, Player* player, int x, int y, int radius, float power);

} // namespace notita
