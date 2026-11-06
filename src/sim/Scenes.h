#pragma once
// Ready-made world setups shared by the app, the benchmark and the tests.
#include "Material.h"
#include "Rng.h"
#include "World.h"

namespace notita::scenes {

// Fill an axis-aligned rect (world pixels, inclusive) with one material.
void fillRect(World& w, MaterialId m, int x0, int y0, int x1, int y1, Rng& rng);

// A small playground: ledges, a basin, piles of powder and pools of liquid.
void demo(World& w, const MaterialRegistry& mats, std::uint64_t seed);

// The whole world filled with a random mix of powders and liquids (and some
// gas), so almost every pixel is moving: the benchmark worst case.
void churn(World& w, const MaterialRegistry& mats, std::uint64_t seed);

// Turn the world upside down (sustained benchmark load: everything falls again).
void flipVertical(World& w);

} // namespace notita::scenes
