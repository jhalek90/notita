#pragma once
// Reaction table: A + B (+ conditions) -> products + heat. Pure data, no
// scripting: data/reactions.luau is evaluated once at load time by
// src/script (sandboxed Luau) and its entries are added here. The tick only
// does table lookups.
//
// When a cell of A touches (4-neighbour) a cell of B, the pair reacts with
// `chance` per tick if A's temperature is within [minTemp, maxTemp]. A becomes
// outA, B becomes outB. The pair's thermal energy plus `heat` is shared by the
// two products, so they come out at one common temperature. Mass must balance:
// mass(A) + mass(B) == mass(outA) + mass(outB), checked when the reaction is added.
#include "Material.h"

#include <string>
#include <vector>

namespace notita {

struct Reaction {
    std::string name;            // for messages / UI ("etchant + iron")
    MaterialId  a = kEmpty, b = kEmpty;
    MaterialId  outA = kEmpty, outB = kEmpty;
    float heat = 0.0f;           // energy released (> 0) or absorbed (< 0)
    float chance = 1.0f;         // per tick, per touching pair
    float minTemp = -kNoPoint;   // conditions on A's temperature
    float maxTemp = kNoPoint;
};

// What the loader hands over (names, not ids yet).
struct ReactionSpec {
    std::string a, b, outA, outB = "air";
    float heat = 0.0f;
    float chance = 1.0f;
    float minTemp = -kNoPoint, maxTemp = kNoPoint;
};

class ReactionTable {
public:
    struct Entry {
        MaterialId partner;
        std::uint16_t index;  // into reactions()
        bool swapped;         // this material is the reaction's B
    };

    explicit ReactionTable(const MaterialRegistry& mats);

    // Validates names and mass balance. Returns false (and appends a message) if rejected.
    bool add(const ReactionSpec& spec, std::string* error = nullptr);

    const std::vector<Reaction>& reactions() const { return reactions_; }
    // Reactions a material takes part in (as A or B). Empty for most materials.
    const std::vector<Entry>& of(MaterialId m) const { return byMaterial_[m]; }
    bool reactive(MaterialId m) const { return !byMaterial_[m].empty(); }
    // Lowest minTemp over the material's reactions (-kNoPoint when one has no lower bound).
    float lowestMinTemp(MaterialId m) const { return lowestMin_[m]; }
    std::size_t size() const { return reactions_.size(); }

private:
    const MaterialRegistry& mats_;
    std::vector<Reaction> reactions_;
    std::vector<std::vector<Entry>> byMaterial_;
    std::vector<float> lowestMin_;
};

} // namespace notita
