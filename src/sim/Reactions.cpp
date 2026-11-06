#include "Reactions.h"

#include <algorithm>

namespace notita {

ReactionTable::ReactionTable(const MaterialRegistry& mats)
    : mats_(mats), byMaterial_(mats.size()), lowestMin_(mats.size(), kNoPoint) {}

bool ReactionTable::add(const ReactionSpec& s, std::string* error) {
    const std::string label = s.a + " + " + s.b;
    auto fail = [&](const std::string& msg) {
        if (error) *error = label + ": " + msg;
        return false;
    };
    const Material* a = mats_.find(s.a);
    const Material* b = mats_.find(s.b);
    const Material* oa = mats_.find(s.outA);
    const Material* ob = mats_.find(s.outB.empty() ? "air" : s.outB);
    if (!a) return fail("unknown material '" + s.a + "'");
    if (!b) return fail("unknown material '" + s.b + "'");
    if (!oa) return fail("unknown product '" + s.outA + "'");
    if (!ob) return fail("unknown product '" + s.outB + "'");
    if (a->id == kEmpty || b->id == kEmpty) return fail("air can't be a reactant (it can be a product)");
    if (a->mass + b->mass != oa->mass + ob->mass)
        return fail("mass is not conserved (" + std::to_string(a->mass + b->mass) + " -> " +
                    std::to_string(oa->mass + ob->mass) + ")");
    if (!(s.chance > 0.0f)) return fail("chance must be > 0");
    if (s.minTemp > s.maxTemp) return fail("min_temp is above max_temp");
    if (reactions_.size() >= 0xFFFF) return fail("too many reactions");
    for (const Entry& e : byMaterial_[a->id])
        if (e.partner == b->id) return fail("this pair already has a reaction");

    Reaction r;
    r.name = label;
    r.a = a->id; r.b = b->id;
    r.outA = oa->id; r.outB = ob->id;
    r.heat = s.heat;
    r.chance = std::min(s.chance, 1.0f);
    r.minTemp = s.minTemp;
    r.maxTemp = s.maxTemp;
    const auto idx = static_cast<std::uint16_t>(reactions_.size());
    reactions_.push_back(r);
    byMaterial_[a->id].push_back({b->id, idx, false});
    if (a->id != b->id) byMaterial_[b->id].push_back({a->id, idx, true});
    // conditions are on A's temperature, so B reacts whenever an A partner qualifies
    lowestMin_[a->id] = std::min(lowestMin_[a->id], r.minTemp);
    lowestMin_[b->id] = -kNoPoint;
    return true;
}

} // namespace notita
