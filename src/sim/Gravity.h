#pragma once
// #483: regions of the world seen from above ("top-down"). Inside a flat
// region gravity points into the screen: powders rest where they are and only
// move when pushed, liquids hold together as puddles and run down the
// region's slope, gases (CA pixels and the gas grid) feel no buoyancy, flames
// flicker any way, rigid bodies slide on the floor with friction and the
// player walks in 8 directions. Everything outside keeps the side view.
//
// The map is part of the scene, not of the cells: a WorldOverlay lists its
// regions (WorldOverlay::flatRegions) and whoever sets the overlay copies them
// here, so streaming and saves need nothing new (a save names its overlay).
// Every system asks per pixel (or per grid cell / body centre) and reads it
// only, so the tick stays deterministic and thread-count independent.
#include <utility>
#include <vector>

namespace notita {

struct FlatRegion {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1; // inclusive world pixels
    // The floor's tilt: liquids gain this much speed per tick downhill
    // (1/256 px per tick^2, like SimParams::gravity). 0 = a level floor.
    int slopeX = 0, slopeY = 0;
    bool contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};

class GravityMap {
public:
    void set(std::vector<FlatRegion> regions) { regions_ = std::move(regions); }
    void clear() { regions_.clear(); }
    bool empty() const { return regions_.empty(); }
    const std::vector<FlatRegion>& regions() const { return regions_; }

    // The region the pixel is in (the first listed wins where they overlap), or null.
    const FlatRegion* at(int x, int y) const {
        for (const FlatRegion& r : regions_)
            if (r.contains(x, y)) return &r;
        return nullptr;
    }
    bool flat(int x, int y) const { return at(x, y) != nullptr; }
    // Any region overlaps the inclusive rect.
    bool touches(int x0, int y0, int x1, int y1) const {
        for (const FlatRegion& r : regions_)
            if (r.x0 <= x1 && r.x1 >= x0 && r.y0 <= y1 && r.y1 >= y0) return true;
        return false;
    }

private:
    std::vector<FlatRegion> regions_;
};

} // namespace notita
