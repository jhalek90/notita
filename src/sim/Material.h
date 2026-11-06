#pragma once
// Data-driven materials. Definitions live in data/materials.txt (INI-like);
// unknown keys are kept in Material::extra so later phases (heat, reactions,
// Luau) can add properties without touching the parser.
#include "Cell.h"

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
    Special, // reserved (fire, energy) for later phases
};

const char* kindName(MaterialKind k);

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

    // Phase 2 (heat) placeholders, parsed now so data files can carry them.
    float conductivity = 0.1f;
    float heatCapacity = 1.0f;

    std::uint32_t color = 0xFF000000; // RGBA8 in memory order (0xAABBGGRR as an int)
    int           colorVariation = 0; // +- per channel range used with Cell::shade

    std::unordered_map<std::string, std::string> extra; // unknown keys, for later phases

    std::array<std::uint32_t, 256> palette{}; // colour per shade, filled by the registry

    bool isFluid() const { return kind == MaterialKind::Empty || kind == MaterialKind::Liquid || kind == MaterialKind::Gas; }
    bool moves() const { return kind == MaterialKind::Powder || kind == MaterialKind::Liquid || kind == MaterialKind::Gas; }
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
    const Material* find(std::string_view name) const;
    MaterialId idOf(std::string_view name) const; // kEmpty when unknown
    std::size_t size() const { return materials_.size(); }
    const std::vector<Material>& all() const { return materials_; }

private:
    void buildPalette(Material& m);
    std::vector<Material> materials_;
    std::unordered_map<std::string, MaterialId> byName_;
};

} // namespace notita
