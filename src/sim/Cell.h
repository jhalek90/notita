#pragma once
// Per-pixel data. Kept at 16 bytes (AoS) so a moving pixel carries all of its
// state with it in a single swap. Later phases fill the reserved fields:
//   temperature -> phase 2 (heat conduction, state changes)
//   life        -> phase 2 (fire / burn timers, lifetimes)
//   vx, vy      -> already used for falling; phase 4 hands fast pixels to the
//                  particle system and back
//   aux         -> free (e.g. rigid body id in phase 4, charge for electricity)
#include <cstdint>

namespace notita {

using MaterialId = std::uint16_t;
constexpr MaterialId kEmpty = 0;   // "air": the material id 0 is always the empty cell

// Velocity is fixed-point: 1/256 pixel per tick.
constexpr int kVelOne = 256;

enum CellFlags : std::uint8_t {
    kFlagNone     = 0,
    kFlagFreeFall = 1 << 0,  // powder that is currently falling (not resting on a pile)
    kFlagBurning  = 1 << 1,  // reserved for phase 2
    kFlagBody     = 1 << 2,  // reserved for phase 4 (pixel owned by a rigid body)
};

struct Cell {
    MaterialId    material = kEmpty;
    std::uint16_t stamp = 0;        // tick stamp of the last update (prevents double moves)
    float         temperature = 20.0f; // degrees C (phase 2)
    std::int16_t  vx = 0;           // 1/256 px per tick
    std::int16_t  vy = 0;           // 1/256 px per tick, +y is down
    std::uint8_t  shade = 128;      // per-pixel colour variation, set on placement
    std::uint8_t  flags = 0;
    std::uint8_t  life = 0;         // reserved (phase 2)
    std::uint8_t  aux = 0;          // reserved
};
static_assert(sizeof(Cell) == 16, "Cell must stay 16 bytes");

} // namespace notita
