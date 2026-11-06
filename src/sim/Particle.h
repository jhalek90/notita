#pragma once
// A pixel that left the grid and flies on its own (phase 4): liquid splashes,
// blast debris, pixels shoved aside by a rigid body. It carries its whole Cell
// (material, temperature, shade) and comes back into the grid when it stops,
// so mass is never lost and its velocity is handed back to the cell.
#include "Cell.h"

namespace notita {

struct Particle {
    float x = 0, y = 0;     // world position in pixels; the cell is (floor(x), floor(y))
    float vx = 0, vy = 0;   // pixels per tick, +y down
    Cell  cell;             // the pixel it carries (stamp/flags cleared)
    std::uint16_t age = 0;  // ticks in flight
    std::uint16_t rest = 0; // ticks spent blocked while waiting for room
};

} // namespace notita
