#include "Material.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace notita {

const char* kindName(MaterialKind k) {
    switch (k) {
    case MaterialKind::Empty: return "empty";
    case MaterialKind::Solid: return "solid";
    case MaterialKind::Powder: return "powder";
    case MaterialKind::Liquid: return "liquid";
    case MaterialKind::Gas: return "gas";
    case MaterialKind::Special: return "special";
    }
    return "?";
}

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool parseFloat(std::string_view v, float& out) {
    std::string tmp(v);
    char* end = nullptr;
    const float f = std::strtof(tmp.c_str(), &end);
    if (end == tmp.c_str() || *end != '\0') return false;
    out = f;
    return true;
}

bool parseInt(std::string_view v, int& out) {
    auto r = std::from_chars(v.data(), v.data() + v.size(), out);
    return r.ec == std::errc() && r.ptr == v.data() + v.size();
}

// "#rrggbb" or "#rrggbbaa" -> RGBA8 in memory order
bool parseColor(std::string_view v, std::uint32_t& out) {
    if (v.empty() || v[0] != '#') return false;
    v.remove_prefix(1);
    if (v.size() != 6 && v.size() != 8) return false;
    std::uint32_t x = 0;
    auto r = std::from_chars(v.data(), v.data() + v.size(), x, 16);
    if (r.ec != std::errc() || r.ptr != v.data() + v.size()) return false;
    std::uint32_t rr, gg, bb, aa = 0xFF;
    if (v.size() == 6) { rr = (x >> 16) & 0xFF; gg = (x >> 8) & 0xFF; bb = x & 0xFF; }
    else { rr = (x >> 24) & 0xFF; gg = (x >> 16) & 0xFF; bb = (x >> 8) & 0xFF; aa = x & 0xFF; }
    out = rr | (gg << 8) | (bb << 16) | (aa << 24);
    return true;
}

bool parseBool(std::string_view v, bool& out) {
    if (v == "true" || v == "yes" || v == "1" || v == "on") out = true;
    else if (v == "false" || v == "no" || v == "0" || v == "off") out = false;
    else return false;
    return true;
}

bool parseKind(std::string_view v, MaterialKind& k) {
    if (v == "solid") k = MaterialKind::Solid;
    else if (v == "powder") k = MaterialKind::Powder;
    else if (v == "liquid") k = MaterialKind::Liquid;
    else if (v == "gas") k = MaterialKind::Gas;
    else if (v == "special") k = MaterialKind::Special;
    else return false;
    return true;
}

// Strip a comment. '#' also starts colour values, so a '#' right after the '='
// or after a comma in a list is part of the value; anywhere else it starts a comment.
std::string_view stripComment(std::string_view line) {
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == ';') return line.substr(0, i);
        if (c == '#') {
            const auto eq = line.find('=');
            const std::string_view before = eq != std::string_view::npos && i > eq ? trim(line.substr(eq + 1, i - eq - 1)) : std::string_view{};
            // the value's first colour, or the next one in a list ("palette = #aaa, #bbb")
            const bool startsValue = eq != std::string_view::npos && i > eq && (before.empty() || before.back() == ',');
            if (!startsValue) return line.substr(0, i);
        }
    }
    return line;
}

// Embedded copy of data/materials.txt (keep in sync), used when the file can't be found.
const char* kEmbeddedMaterials = R"MAT(
[bedrock]
kind = solid
density = 5000
anchor = true
conductivity = 0.05
heat_capacity = 2.0
color = #2e2a33
color_variation = 6

[slate]
kind = solid
density = 2700
conductivity = 0.2
heat_capacity = 1.0
color = #6b6f78
color_variation = 14

[sand]
kind = powder
density = 1600
slide = 0.85
conductivity = 0.1
heat_capacity = 0.8
melt_point = 1150
melts_into = molten_glass
color = #d8b86a
color_variation = 18

[cinder_grit]
kind = powder
density = 2400
slide = 0.45
conductivity = 0.12
heat_capacity = 0.9
color = #7a5a4c
color_variation = 22

[water]
kind = liquid
density = 1000
viscosity = 0.0
spread = 6
conductivity = 0.3
heat_capacity = 1.0
boil_point = 100
boils_into = steam
latent_heat = 8
freeze_point = 0
freezes_into = frost
miscible = etchant, tincture
color = #3a78c8c8
color_variation = 8

[lamp_oil]
kind = liquid
density = 820
viscosity = 0.15
spread = 5
conductivity = 0.12
heat_capacity = 0.6
ignite_point = 230
burn_time = 90
burn_heat = 14
flame_chance = 0.45
burns_into = smoke
color = #9c7a2ed0
color_variation = 10

[amber_syrup]
kind = liquid
density = 1400
viscosity = 0.85
spread = 2
conductivity = 0.1
heat_capacity = 1.2
ignite_point = 320
burn_time = 160
burn_heat = 8
flame_chance = 0.2
burns_into = smoke
burn_residue = ash
residue_chance = 0.5
color = #c8661ce8
color_variation = 10

[quickmetal]
kind = liquid
density = 13500
viscosity = 0.05
spread = 5
conductivity = 0.7
heat_capacity = 0.4
color = #c4ccd6
color_variation = 12

[steam]
kind = gas
density = 0.6
spread = 2
conductivity = 0.05
heat_capacity = 0.5
spawn_temp = 110
condense_point = 70
condenses_into = water
color = #c8d2dc90
color_variation = 12

[murk]
kind = gas
density = 3.5
spread = 2
conductivity = 0.04
heat_capacity = 0.4
ignite_point = 400
burn_time = 4
burn_heat = 40
flame_chance = 0.6
burns_into = smoke
color = #5c7a3ca0
color_variation = 14

; ---- phase 2: heat, fire and chemistry ----

[frost]
label = Frost
kind = solid
density = 917
conductivity = 0.35
heat_capacity = 0.9
spawn_temp = -30
melt_point = 2
melts_into = water
color = #bfe3f2e8
color_variation = 10

[magma]
kind = liquid
density = 2800
viscosity = 0.92
spread = 2
conductivity = 0.25
heat_capacity = 1.2
spawn_temp = 1300
freeze_point = 750
freezes_into = basalt
color = #e8561c
color_variation = 26

[basalt]
kind = solid
density = 2900
conductivity = 0.2
heat_capacity = 1.2
melt_point = 950
melts_into = magma
color = #3b3533
color_variation = 12

[molten_glass]
kind = liquid
density = 2400
viscosity = 0.9
spread = 2
conductivity = 0.15
heat_capacity = 0.8
spawn_temp = 1250
freeze_point = 850
freezes_into = glass
color = #f2a648
color_variation = 16

[glass]
kind = solid
density = 2500
conductivity = 0.12
heat_capacity = 0.8
melt_point = 1050
melts_into = molten_glass
color = #a8d8d070
color_variation = 6

[timber]
kind = solid
density = 600
conductivity = 0.04
heat_capacity = 1.2
ignite_point = 300
burn_time = 230
burn_heat = 16
flame_chance = 0.22
burns_into = smoke
burn_residue = ash
residue_chance = 0.35
color = #7b5232
color_variation = 18

[ash]
kind = powder
density = 500
slide = 0.6
conductivity = 0.05
heat_capacity = 0.6
color = #8c8780
color_variation = 16

[smoke]
kind = gas
density = 0.9
spread = 2
conductivity = 0.04
heat_capacity = 0.4
spawn_temp = 300
condense_point = 45
condenses_into = ash
change_chance = 0.01
color = #4a4648a0
color_variation = 16

[flame]
kind = special
density = 0.3
conductivity = 0.6
heat_capacity = 0.3
spawn_temp = 950
life_min = 10
life_max = 34
color = #ff9a2a
color_variation = 40

[etchant]
kind = liquid
density = 1150
viscosity = 0.05
spread = 5
conductivity = 0.25
heat_capacity = 1.0
boil_point = 140
boils_into = etchant_fume
latent_heat = 6
color = #9be04ad0
color_variation = 12

[etchant_fume]
label = Etchant fume
kind = gas
density = 2.2
spread = 2
conductivity = 0.05
heat_capacity = 0.5
spawn_temp = 150
condense_point = 110
condenses_into = etchant
color = #b8e888a0
color_variation = 12

[iron]
kind = solid
density = 7800
conductivity = 0.85
heat_capacity = 0.5
color = #8a8f96
color_variation = 10

[rust_salt]
label = Rust salt
kind = powder
density = 1900
slide = 0.7
conductivity = 0.1
heat_capacity = 0.8
color = #b0603a
color_variation = 20

[hydrogen]
kind = gas
density = 0.09
spread = 3
conductivity = 0.1
heat_capacity = 0.3
ignite_point = 500
burn_time = 2
burn_heat = 80
burn_max_temp = 1400
flame_chance = 0.9
burns_into = steam
color = #e6ecff50
color_variation = 6

; ---- phase 4: liquids that blend ----

[tincture]
label = Tincture
kind = liquid
density = 1040
viscosity = 0.02
spread = 6
conductivity = 0.3
heat_capacity = 1.0
boil_point = 105
boils_into = steam
latent_heat = 8
miscible = water
color = #5a2a8ad8
color_variation = 10
)MAT";

} // namespace

MaterialRegistry::MaterialRegistry() {
    Material air;
    air.id = kEmpty;
    air.name = "air";
    air.label = "Air";
    air.kind = MaterialKind::Empty;
    air.density = 1.2f;
    air.color = 0xFF1A1210; // dark backdrop
    air.mass = 0;
    air.conductivity = 0.03f; // air barely conducts; the phase 3 gas grid adds convection
    air.heatCapacity = 0.25f;
    buildPalette(air);
    materials_.push_back(air);
    byName_["air"] = kEmpty;
    miscible_.assign(1, 0);
}

const Material* MaterialRegistry::find(std::string_view name) const {
    auto it = byName_.find(std::string(name));
    return it == byName_.end() ? nullptr : &materials_[it->second];
}

MaterialId MaterialRegistry::idOf(std::string_view name) const {
    auto it = byName_.find(std::string(name));
    return it == byName_.end() ? kEmpty : it->second;
}

void MaterialRegistry::buildPalette(Material& m) {
    if (m.texture.enabled) {
        buildTexturePalette(m.texture, m.colorVariation, m.palette);
        return;
    }
    const int r = m.color & 0xFF, g = (m.color >> 8) & 0xFF, b = (m.color >> 16) & 0xFF;
    const std::uint32_t a = m.color & 0xFF000000u;
    for (int s = 0; s < 256; ++s) {
        const int off = ((s - 128) * m.colorVariation) / 128;
        auto c = [](int v) { return static_cast<std::uint32_t>(std::clamp(v, 0, 255)); };
        // blue moves a little less than red/green: darker shades lean cool, lighter warm
        m.palette[s] = c(r + off) | (c(g + off) << 8) | (c(b + off - off / 4) << 16) | a;
    }
}

bool MaterialRegistry::loadFromString(std::string_view text, std::vector<std::string>* errors) {
    bool ok = true;
    auto fail = [&](int line, const std::string& msg) {
        ok = false;
        if (errors) errors->push_back("line " + std::to_string(line) + ": " + msg);
    };

    Material cur;
    bool have = false;
    int sectionLine = 0;

    auto commit = [&]() {
        if (!have) return;
        have = false;
        if (cur.name == "air") {
            // only air's thermal properties can be tuned; it stays id 0, empty, massless
            if (cur.kind != MaterialKind::Empty) { fail(sectionLine, "'air' is built in; only conductivity and heat_capacity can be set"); return; }
            materials_[kEmpty].conductivity = std::clamp(cur.conductivity, 0.0f, 1.0f);
            materials_[kEmpty].heatCapacity = std::max(cur.heatCapacity, 0.01f);
            return;
        }
        if (cur.kind == MaterialKind::Empty) { fail(sectionLine, "material '" + cur.name + "' needs a kind"); return; }
        if (materials_.size() >= 0xFFFF) { fail(sectionLine, "too many materials"); return; }
        cur.spread = std::clamp(cur.spread, 0, 16);
        cur.maxFall = std::clamp(cur.maxFall, 1, 16);
        cur.viscosity = std::clamp(cur.viscosity, 0.0f, 1.0f);
        cur.slide = std::clamp(cur.slide, 0.0f, 1.0f);
        cur.colorVariation = std::clamp(cur.colorVariation, 0, 128);
        if (cur.density <= 0.0f) cur.density = 0.01f;
        cur.conductivity = std::clamp(cur.conductivity, 0.0f, 1.0f);
        cur.heatCapacity = std::max(cur.heatCapacity, 0.01f);
        cur.spawnTemp = std::clamp(cur.spawnTemp, kMinTemp, kMaxTemp);
        cur.mass = std::max(cur.mass, 0);
        cur.changeChance = std::clamp(cur.changeChance, 0.0f, 1.0f);
        cur.burnTime = std::clamp(cur.burnTime, 0, 255);
        cur.flameChance = std::clamp(cur.flameChance, 0.0f, 1.0f);
        cur.residueChance = std::clamp(cur.residueChance, 0.0f, 1.0f);
        cur.lifeMin = std::clamp(cur.lifeMin, 1, 255);
        cur.lifeMax = std::clamp(cur.lifeMax, cur.lifeMin, 255);
        if (cur.kind == MaterialKind::Special) cur.mass = 0; // flames carry energy, not matter
        if (cur.label.empty()) {
            cur.label = cur.name;
            std::replace(cur.label.begin(), cur.label.end(), '_', ' ');
            cur.label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cur.label[0])));
        }
        {
            std::string err;
            if (!parseTexture(cur.extra, cur.color, cur.texture, &err)) fail(sectionLine, "material '" + cur.name + "': " + err);
        }
        auto it = byName_.find(cur.name);
        if (it != byName_.end()) {
            cur.id = it->second;
            buildPalette(cur);
            materials_[cur.id] = cur;
        } else {
            cur.id = static_cast<MaterialId>(materials_.size());
            byName_[cur.name] = cur.id;
            buildPalette(cur);
            materials_.push_back(cur);
        }
    };

    int lineNo = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = trim(stripComment(text.substr(pos, nl - pos)));
        pos = nl + 1;
        ++lineNo;
        if (line.empty()) continue;

        if (line.front() == '[') {
            commit();
            if (line.back() != ']') { fail(lineNo, "bad section header"); continue; }
            std::string name(trim(line.substr(1, line.size() - 2)));
            if (name.empty()) { fail(lineNo, "empty material name"); continue; }
            cur = Material{};
            if (const Material* existing = find(name)) cur = *existing; // overrides keep old values
            cur.name = name;
            cur.label.clear();
            have = true;
            sectionLine = lineNo;
            continue;
        }

        auto eq = line.find('=');
        if (eq == std::string_view::npos) { fail(lineNo, "expected key = value"); continue; }
        if (!have) { fail(lineNo, "key outside a [material] section"); continue; }
        std::string key(trim(line.substr(0, eq)));
        std::string_view val = trim(line.substr(eq + 1));

        bool good = true;
        if (key == "kind") good = parseKind(val, cur.kind);
        else if (key == "label") cur.label = std::string(val);
        else if (key == "density") good = parseFloat(val, cur.density);
        else if (key == "viscosity") good = parseFloat(val, cur.viscosity);
        else if (key == "spread") good = parseInt(val, cur.spread);
        else if (key == "slide") good = parseFloat(val, cur.slide);
        else if (key == "gravity") good = parseFloat(val, cur.gravity);
        else if (key == "max_fall") good = parseInt(val, cur.maxFall);
        else if (key == "conductivity") good = parseFloat(val, cur.conductivity);
        else if (key == "heat_capacity") good = parseFloat(val, cur.heatCapacity);
        else if (key == "spawn_temp" || key == "temperature") good = parseFloat(val, cur.spawnTemp);
        else if (key == "mass") good = parseInt(val, cur.mass);
        else if (key == "melt_point" || key == "boil_point") good = parseFloat(val, cur.hot.point);
        else if (key == "melts_into" || key == "boils_into") cur.hot.intoName = std::string(val);
        else if (key == "freeze_point" || key == "condense_point") good = parseFloat(val, cur.cold.point);
        else if (key == "freezes_into" || key == "condenses_into") cur.cold.intoName = std::string(val);
        else if (key == "latent_heat") good = parseFloat(val, cur.latentHeat);
        else if (key == "change_chance") good = parseFloat(val, cur.changeChance);
        else if (key == "ignite_point") good = parseFloat(val, cur.ignitePoint);
        else if (key == "burn_time") good = parseInt(val, cur.burnTime);
        else if (key == "burn_heat") good = parseFloat(val, cur.burnHeat);
        else if (key == "burn_max_temp") good = parseFloat(val, cur.burnMaxTemp);
        else if (key == "flame_chance") good = parseFloat(val, cur.flameChance);
        else if (key == "burns_into") cur.burnsInto.intoName = std::string(val);
        else if (key == "burn_residue") cur.burnResidue.intoName = std::string(val);
        else if (key == "residue_chance") good = parseFloat(val, cur.residueChance);
        else if (key == "flame") cur.flameName = std::string(val);
        else if (key == "life_min") good = parseInt(val, cur.lifeMin);
        else if (key == "life_max") good = parseInt(val, cur.lifeMax);
        else if (key == "miscible") {
            cur.miscibleNames.clear();
            std::string_view rest = val;
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                const std::string_view item = trim(rest.substr(0, comma));
                if (!item.empty()) cur.miscibleNames.emplace_back(item);
                if (comma == std::string_view::npos) break;
                rest.remove_prefix(comma + 1);
            }
        }
        else if (key == "anchor") good = parseBool(val, cur.anchor);
        else if (key == "rigid") good = parseBool(val, cur.rigid);
        else if (key == "friction") good = parseFloat(val, cur.friction);
        else if (key == "bounce") good = parseFloat(val, cur.bounce);
        else if (key == "color") good = parseColor(val, cur.color);
        else if (key == "color_variation") good = parseInt(val, cur.colorVariation);
        else cur.extra[key] = std::string(val);
        if (!good) fail(lineNo, "bad value for '" + key + "': " + std::string(val));
    }
    commit();
    resolve(errors, ok);
    return ok;
}

void MaterialRegistry::resolve(std::vector<std::string>* errors, bool& ok) {
    auto fail = [&](const Material& m, const std::string& msg) {
        ok = false;
        if (errors) errors->push_back("material '" + m.name + "': " + msg);
    };
    // a product must exist and carry the same mass, or the change is switched off
    auto link = [&](Material& m, Transition& t, const char* what, bool needsPoint) {
        t.valid = false;
        t.into = kEmpty;
        if (t.intoName.empty()) {
            if (needsPoint && std::abs(t.point) < kNoPoint) fail(m, std::string(what) + " has a temperature but no product");
            return;
        }
        if (needsPoint && std::abs(t.point) >= kNoPoint) { fail(m, std::string(what) + " product '" + t.intoName + "' has no temperature"); return; }
        const auto it = byName_.find(t.intoName);
        if (it == byName_.end()) { fail(m, std::string(what) + " product '" + t.intoName + "' is not a material"); return; }
        const Material& into = materials_[it->second];
        if (into.mass != m.mass) {
            fail(m, std::string(what) + " into '" + into.name + "' would change mass (" + std::to_string(m.mass) + " -> " +
                        std::to_string(into.mass) + ")");
            return;
        }
        t.into = into.id;
        t.valid = true;
    };
    for (Material& m : materials_) {
        if (m.id == kEmpty) continue;
        if (m.hot.point >= kNoPoint) m.hot.point = kNoPoint;
        if (m.cold.point >= kNoPoint || m.cold.point <= -kNoPoint) m.cold.point = -kNoPoint;
        link(m, m.hot, "hot change", true);
        link(m, m.cold, "cold change", true);
        if (m.hot.valid && m.cold.valid && m.cold.point >= m.hot.point)
            fail(m, "freeze/condense point must be below its melt/boil point");
        m.flame = kEmpty;
        if (m.ignitePoint < kNoPoint) {
            link(m, m.burnsInto, "burning", false);
            if (!m.burnsInto.valid && m.burnsInto.intoName.empty()) fail(m, "ignite_point needs burns_into");
            if (m.burnTime <= 0) fail(m, "ignite_point needs burn_time > 0");
            if (!m.burnResidue.intoName.empty()) link(m, m.burnResidue, "burn residue", false);
            if (const Material* f = find(m.flameName); f && f->kind == MaterialKind::Special) m.flame = f->id;
        }
    }
    // miscibility: liquids only, symmetric
    const std::size_t n = materials_.size();
    miscible_.assign(n * n, 0);
    for (Material& m : materials_) {
        m.miscibleWith.clear();
        for (const std::string& name : m.miscibleNames) {
            const auto it = byName_.find(name);
            if (it == byName_.end()) { fail(m, "miscible with '" + name + "', which is not a material"); continue; }
            const Material& o = materials_[it->second];
            if (m.kind != MaterialKind::Liquid || o.kind != MaterialKind::Liquid) {
                fail(m, "miscible with '" + name + "': only liquids blend");
                continue;
            }
            if (o.id == m.id) continue;
            miscible_[static_cast<std::size_t>(m.id) * n + o.id] = 1;
            miscible_[static_cast<std::size_t>(o.id) * n + m.id] = 1;
        }
    }
    for (Material& m : materials_)
        for (std::size_t j = 0; j < n; ++j)
            if (miscible_[static_cast<std::size_t>(m.id) * n + j]) m.miscibleWith.push_back(static_cast<MaterialId>(j));
}

bool MaterialRegistry::loadFromFile(const std::string& path, std::vector<std::string>* errors) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (errors) errors->push_back("cannot open " + path);
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return loadFromString(ss.str(), errors);
}

MaterialRegistry MaterialRegistry::loadDefault(std::vector<std::string>* errors) {
    MaterialRegistry reg;
    const char* candidates[] = {"data/materials.txt", "../data/materials.txt",
#ifdef NOTITA_DATA_DIR
                                NOTITA_DATA_DIR "/materials.txt",
#endif
    };
    for (const char* path : candidates) {
        std::ifstream probe(path);
        if (probe) {
            reg.loadFromFile(path, errors);
            return reg;
        }
    }
    reg.loadFromString(kEmbeddedMaterials, errors);
    return reg;
}

} // namespace notita
