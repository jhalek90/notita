#pragma once
// Ready-made world setups shared by the app, the benchmark and the tests.
#include "Material.h"
#include "Rng.h"
#include "World.h"

namespace notita::scenes {

// Fill an axis-aligned rect (world pixels, inclusive) with one material.
void fillRect(World& w, MaterialId m, int x0, int y0, int x1, int y1, Rng& rng, float temperature = kAmbientTemp);
// Same, at the material's spawn temperature (magma comes out hot, frost cold).
void fillSpawn(World& w, const MaterialRegistry& mats, MaterialId m, int x0, int y0, int x1, int y1, Rng& rng);

// A small playground: ledges, a basin, piles of powder and pools of liquid.
void demo(World& w, const MaterialRegistry& mats, std::uint64_t seed);

// The whole world filled with a random mix of powders and liquids (and some
// gas), so almost every pixel is moving: the benchmark worst case.
void churn(World& w, const MaterialRegistry& mats, std::uint64_t seed);

// Heat playground: magma with water dripping on it, a timber hut over lamp
// oil that is set alight, etchant on an iron plate, frost and red-hot sand.
void forge(World& w, const MaterialRegistry& mats, std::uint64_t seed);

// churn, but the mix includes magma, frost, timber, etchant and iron at their
// spawn temperatures: boiling, freezing, burning and reactions everywhere
// (the benchmark worst case with heat on).
void thermalChurn(World& w, const MaterialRegistry& mats, std::uint64_t seed);

// Turn the world upside down (sustained benchmark load: everything falls again).
void flipVertical(World& w);

} // namespace notita::scenes
