#pragma once
// Phase 6: the procedural world. Every pixel is a pure function of (seed, x, y),
// so any chunk can be generated on its own, in any order, and borders line up.
//
// Layout (+y is down, the surface hovers around y = 0, unbounded both ways):
//   sky above a rolling surface; biomes along x (meadow: turf over loam,
//   dunes: deep sand, ashlands: cinder grit over basalt); slate below with
//   iron veins, basalt deep down; caves (winding tunnels and caverns); cave
//   space below the local water table is flooded, deep or volcanic caverns
//   hold magma, some dry caves hold hydrogen (shallow) or murk (deep), and
//   lamp oil sits in pockets just above some water tables.
// Solids are pinned (kAuxPinned: scene geometry, never a rigid body);
// powders and liquids are free and settle when the chunk first runs.
#include "Chunk.h"
#include "Material.h"

#include <cstdint>

namespace notita {

enum class Biome : std::uint8_t { Meadow, Dunes, Ashlands, Count };
const char* biomeName(Biome b);

// A hand-built area laid over the generated world (the showcase building).
// Like the generator it must be a pure function of (x, y): any chunk order,
// same result. It may replace cells and flatten the surface under itself.
class WorldOverlay {
public:
    virtual ~WorldOverlay() = default;
    virtual const char* name() const = 0;                  // stored in saves ("" = none)
    virtual int surface(int x, int generated) const = 0;   // the column's surface (flatten a site)
    virtual bool cellAt(int x, int y, Cell& out) const = 0; // true: use `out` instead of the generated cell
};

struct WorldGenParams {
    float surfaceAmp = 110.0f;     // px, large rolling hills
    float hillAmp = 24.0f;         // px, small bumps
    int   caveDepth = 40;          // no caves this close to the surface (dunes: sand needs a floor)
    float tunnelWidth = 0.055f;    // |noise| below this is tunnel
    float cavernLevel = 0.42f;     // noise above this is a cavern
    int   waterTable = 230;        // px below the surface, +- waterVariation
    int   waterVariation = 120;
    int   lavaDepth = 420;         // magma caverns below this depth (ashlands: much shallower)
    int   basaltDepth = 700;
};

class WorldGen {
public:
    WorldGen(const MaterialRegistry& mats, std::uint64_t seed, WorldGenParams params = {});

    // Fills the chunk's cells (all of them).
    void generate(Chunk& chunk) const;
    // The cell at a world pixel (what generate() writes there).
    Cell cellAt(int x, int y) const;

    int surfaceY(int x) const;     // first ground pixel of the column
    Biome biome(int x) const;
    int waterTableY(int x) const;
    std::uint64_t seed() const { return seed_; }
    // An overlay replaces cells (and the surface) where it wants to. Set it
    // before the first chunk is generated: chunks already made keep their cells.
    void setOverlay(const WorldOverlay* overlay) { overlay_ = overlay; }
    const WorldOverlay* overlay() const { return overlay_; }
    const WorldGenParams& params() const { return params_; }

private:
    struct Column { int surface; int water; Biome biome; float blend; };
    Column column(int x) const;
    Cell cellIn(const Column& col, int x, int y) const;
    float noise1(float x, std::uint64_t salt) const;
    float noise2(float x, float y, std::uint64_t salt) const;
    float fbm1(float x, int octaves, std::uint64_t salt) const;
    float fbm2(float x, float y, int octaves, std::uint64_t salt) const;
    Cell make(MaterialId m, int x, int y) const;

    const MaterialRegistry& mats_;
    std::uint64_t seed_;
    WorldGenParams params_;
    const WorldOverlay* overlay_ = nullptr;
    MaterialId slate_, basalt_, sand_, grit_, loam_, turf_, iron_, water_, magma_, hydrogen_, murk_, oil_, bedrock_;
};

} // namespace notita
