#pragma once
// Sandbox brushes: place / replace / erase any material, or heat / cool, with a
// circle or square. Placed cells start at their material's spawn temperature.
#include "Material.h"
#include "Rng.h"
#include "World.h"

namespace notita {

enum class BrushShape { Circle, Square };
enum class BrushMode {
    Place,   // fill only cells that hold air or a gas
    Replace, // overwrite everything
    Erase,   // set to air
    Heat,    // raise the temperature of whatever is there
    Cool,    // lower it
};

struct Brush {
    MaterialId material = kEmpty;
    int        radius = 4;          // 0 = single pixel
    BrushShape shape = BrushShape::Circle;
    BrushMode  mode = BrushMode::Place;
    float      density = 1.0f;      // fraction of cells painted per dab (spray when < 1)
    float      heat = 25.0f;        // Heat / Cool: degrees per dab at the centre (fades to 1/4 at the rim)
};

// One dab centred at (x, y). Returns the number of cells changed.
int applyBrush(World& world, const MaterialRegistry& mats, const Brush& brush, int x, int y, Rng& rng);

// Dabs along the line from (x0, y0) to (x1, y1) so fast strokes leave no gaps.
int strokeBrush(World& world, const MaterialRegistry& mats, const Brush& brush,
                int x0, int y0, int x1, int y1, Rng& rng);

} // namespace notita
