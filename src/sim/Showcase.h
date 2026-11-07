#pragma once
// The showcase (--scene showcase, the default scene): a concrete building with
// timber beams laid over the procedural world, one room per system.
//
//   ground floor: sand silo | fluids (tanks, valves, a drain to the cistern) |
//                 lobby (spawn, stairs) | electrical room | furnace + chimney
//   first floor:  silo top  | gas chamber (hydrogen, igniter, blast door) |
//                 gallery | demolition hall (beams holding loose blocks) | flue
//   basement:     the cistern under the fluids room
//   outside:      a cave down to a water table (west), a movement course with
//                 a jet tower, a grapple gantry and a raft pond (east)
//   past the course: the TOP DOWN room (#483), a walled box seen from above
//                 (a FlatRegion: gravity into the screen) with a tiled floor, a
//                 pond, a sand pile, a timber hut with an igniter, a steam vent,
//                 a sloped gutter under a water tank, a lamp behind a switch and
//                 a crate; its door is in the west wall at ground level
//
// It is a WorldOverlay: every pixel is a pure function of (x, y) (a raster
// built once in the constructor), so the building streams, sleeps and saves
// exactly like the generated terrain around it. The surface is flattened to
// y = 0 under the site and blends back into the generated hills outside.
#include "Material.h"
#include "WorldGen.h"

#include <cstdint>
#include <string>
#include <vector>

namespace notita {

class Showcase final : public WorldOverlay {
public:
    explicit Showcase(const MaterialRegistry& mats);

    const char* name() const override { return "showcase"; }
    int surface(int x, int generated) const override;
    bool cellAt(int x, int y, Cell& out) const override;
    std::vector<FlatRegion> flatRegions() const override { return flat_; }

    // The player's spawn point (feet), in the lobby.
    float spawnX() const { return spawnX_; }
    float spawnY() const { return spawnY_; }
    // #483: where --scene topdown puts the player (feet), just inside the room's door.
    float topDownX() const { return topDownX_; }
    float topDownY() const { return topDownY_; }

    struct Room { std::string name; int x0, y0, x1, y1; }; // interior, world pixels
    const std::vector<Room>& rooms() const { return rooms_; }
    const Room* room(const std::string& name) const;

    // The raster's bounds (world pixels, inclusive).
    int minX() const { return x0_; }
    int minY() const { return y0_; }
    int maxX() const { return x0_ + w_ - 1; }
    int maxY() const { return y0_ + h_ - 1; }

private:
    static constexpr std::uint16_t kGenerated = 0xFFFF; // leave the generator's cell
    enum Hold : std::uint8_t { Pinned = 0, Loose = 1, Live = 2 }; // solids: scene geometry, settled loose pixels, a fresh clump
    void build();

    const MaterialRegistry& mats_;
    int x0_ = 0, y0_ = 0, w_ = 0, h_ = 0;
    std::vector<std::uint16_t> mat_;
    std::vector<std::uint8_t> shade_; // 0 = the terrain hash
    std::vector<std::uint8_t> hold_;
    std::vector<Room> rooms_;
    float spawnX_ = 0, spawnY_ = 0;
    float topDownX_ = 0, topDownY_ = 0;
    std::vector<FlatRegion> flat_;
    friend struct ShowcasePainter;
};

} // namespace notita
