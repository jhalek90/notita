#pragma once
// Data-driven materials. Definitions live in data/materials.txt (INI-like);
// unknown keys are kept in Material::extra so later phases can add properties
// without touching the parser. Phase 2 adds heat, state changes and fire;
// reactions between materials live in data/reactions.luau (see Reactions.h).
#include "Cell.h"
#include "Texture.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace notita {

enum class MaterialKind : std::uint8_t {
    Empty,   // id 0, "air"
    Solid,   // static
    Powder,  // falls, piles, sinks through lighter fluids
    Liquid,  // falls, spreads sideways, density-sorted, viscosity
    Gas,     // rises or sinks by density (CA gas until the phase 3 GPU grid)
    Special, // flames: short-lived, massless, hot; rise through air (energy later)
};

const char* kindName(MaterialKind k);

constexpr float kAmbientTemp = 20.0f;   // degrees C; fresh cells start here
constexpr float kMinTemp = -273.0f;
constexpr float kMaxTemp = 10000.0f;
constexpr float kNoPoint = 1.0e30f;      // "no threshold" for hot/cold points

// A temperature-driven state change (melt/boil going up, freeze/condense going down).
struct Transition {
    float       point = kNoPoint;  // +kNoPoint (hot) / -kNoPoint (cold) when unused
    std::string intoName;          // as written in the data file
    MaterialId  into = kEmpty;     // resolved by the registry
    bool        valid = false;     // false when unused or rejected (unknown name, mass mismatch)
};

struct Material {
    MaterialId   id = kEmpty;
    std::string  name;            // key used in data files and code ("sand")
    std::string  label;           // shown in the UI
    MaterialKind kind = MaterialKind::Empty;

    float density = 1.2f;         // kg/m^3-ish; drives sinking / floating
    float viscosity = 0.0f;       // liquids: 0 = runs freely, 1 = barely spreads
    int   spread = 4;             // liquids/gases: max sideways pixels per tick (0..16)
    float slide = 0.9f;           // powders: chance per tick to slide diagonally (pile angle)
    float gravity = 1.0f;         // gravity scale
    int   maxFall = 12;           // terminal speed, pixels per tick (1..16)

    // ---- heat (phase 2) ----
    // Conduction between two cells uses the harmonic mean of their
    // conductivities; heat_capacity is per cell (energy per degree), so energy
    // is sum(heat_capacity * temperature) and conduction keeps it constant.
    float conductivity = 0.1f;    // 0..1
    float heatCapacity = 1.0f;    // > 0
    float spawnTemp = kAmbientTemp; // temperature of freshly placed cells
    // Mass per cell, in whole units. Every state change, burn and reaction
    // must turn N units into N units (checked at load), so one water pixel
    // boils into one steam pixel and sealed rooms keep their gas. Air = 0.
    int   mass = 1;

    Transition hot;               // melt_point/boil_point + melts_into/boils_into
    Transition cold;              // freeze_point/condense_point + freezes_into/condenses_into
    float latentHeat = 0.0f;      // energy absorbed by the hot transition (released going back)
    float changeChance = 0.35f;   // chance per tick to change once past a threshold

    // ---- fire ----
    float ignitePoint = kNoPoint; // starts burning at this temperature when it touches air
    int   burnTime = 0;           // ticks of burning (1..255)
    float burnHeat = 0.0f;        // energy released per tick while burning
    float burnMaxTemp = 900.0f;   // burning stops adding heat above this
    float flameChance = 0.25f;    // chance per tick to throw a flame into the air next to it
    Transition burnsInto;         // what a burnt-out cell becomes (point unused)
    Transition burnResidue;       // optional alternative product (ash)...
    float residueChance = 0.0f;   // ...picked with this chance
    std::string flameName = "flame"; // the flame material it throws
    MaterialId  flame = kEmpty;

    // ---- special: flames ----
    int lifeMin = 12, lifeMax = 30; // flame lifetime in ticks

    // ---- phase 4: liquids and rigid bodies ----
    // Liquids listed here blend with this one (random interdiffusion, no
    // density layering) instead of layering. Symmetric after resolve().
    std::vector<std::string> miscibleNames;
    std::vector<MaterialId>  miscibleWith;
    // Solids: an anchor holds everything connected to it in place (bedrock);
    // a clump of solid pixels with no anchor and nothing under it becomes a
    // rigid body. rigid = false keeps a solid static no matter what.
    bool anchor = false;
    bool rigid = true;
    float friction = 0.6f;        // Box2D surface friction of a body made of it
    float bounce = 0.05f;         // Box2D restitution

    std::uint32_t color = 0xFF000000; // RGBA8 in memory order (0xAABBGGRR as an int)
    int           colorVariation = 0; // +- per channel range used with Cell::shade

    std::unordered_map<std::string, std::string> extra; // unknown keys, for later phases

    // #481: per-pixel texture (palette + layered noise, keys in data/materials.txt);
    // disabled = the old brightness ramp with a hashed shade
    MaterialTexture texture;

    std::array<std::uint32_t, 256> palette{}; // colour per shade, filled by the registry

    // things others can move through (flames count: sand falls through fire)
    bool isFluid() const {
        return kind == MaterialKind::Empty || kind == MaterialKind::Liquid || kind == MaterialKind::Gas ||
               kind == MaterialKind::Special;
    }
    bool moves() const { return kind == MaterialKind::Powder || kind == MaterialKind::Liquid || kind == MaterialKind::Gas; }
    bool flammable() const { return ignitePoint < kNoPoint && burnTime > 0 && burnsInto.valid; }
    bool isFlame() const { return kind == MaterialKind::Special; }
    // The shade a freshly made pixel of this material gets at (x, y): its texture
    // sampled in world space (along = beam direction for grain, 0 = x, 1 = y).
    std::uint8_t shadeAt(int x, int y, int along = 0) const { return textureShade(texture, x, y, along); }
    // a cell of this material with its spawn temperature
    Cell spawn(std::uint8_t shade) const {
        Cell c;
        c.material = id;
        c.shade = shade;
        c.temperature = spawnTemp;
        return c;
    }
};

class MaterialRegistry {
public:
    MaterialRegistry();  // contains only "air" (id 0)

    // Parse INI-like text. Returns false and fills errors on problems; good
    // sections are still added. Re-defining a name updates it in place.
    bool loadFromString(std::string_view text, std::vector<std::string>* errors = nullptr);
    bool loadFromFile(const std::string& path, std::vector<std::string>* errors = nullptr);

    // Registry loaded from data/materials.txt, falling back to the embedded copy.
    static MaterialRegistry loadDefault(std::vector<std::string>* errors = nullptr);

    const Material& operator[](MaterialId id) const { return materials_[id]; }
    // Do these two liquids blend (symmetric)?
    bool miscible(MaterialId a, MaterialId b) const {
        const std::size_t n = materials_.size();
        return a < n && b < n && miscible_[a * n + b] != 0;
    }
    const std::vector<std::uint8_t>& miscibleTable() const { return miscible_; } // size() * size()
    const Material* find(std::string_view name) const;
    MaterialId idOf(std::string_view name) const; // kEmpty when unknown
    std::size_t size() const { return materials_.size(); }
    const std::vector<Material>& all() const { return materials_; }

private:
    void buildPalette(Material& m);
    // Resolves transition/burn names to ids and checks that mass is conserved.
    void resolve(std::vector<std::string>* errors, bool& ok);
    std::vector<Material> materials_;
    std::unordered_map<std::string, MaterialId> byName_;
    std::vector<std::uint8_t> miscible_;
};

} // namespace notita
