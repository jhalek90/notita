#include "Material.h"

#include <algorithm>
#include <cctype>
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

bool parseKind(std::string_view v, MaterialKind& k) {
    if (v == "solid") k = MaterialKind::Solid;
    else if (v == "powder") k = MaterialKind::Powder;
    else if (v == "liquid") k = MaterialKind::Liquid;
    else if (v == "gas") k = MaterialKind::Gas;
    else if (v == "special") k = MaterialKind::Special;
    else return false;
    return true;
}

// Strip a comment. '#' also starts colour values, so a '#' only starts a
// comment at the beginning of a line or after whitespace once a value exists.
std::string_view stripComment(std::string_view line) {
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == ';') return line.substr(0, i);
        if (c == '#') {
            const auto eq = line.find('=');
            const bool startsValue = eq != std::string_view::npos && i > eq &&
                                     trim(line.substr(eq + 1, i - eq - 1)).empty();
            if (!startsValue) return line.substr(0, i);
        }
    }
    return line;
}

// Embedded copy of data/materials.txt, used when the file can't be found.
const char* kEmbeddedMaterials = R"MAT(
[bedrock]
kind = solid
density = 5000
color = #2e2a33
color_variation = 6

[slate]
kind = solid
density = 2700
color = #6b6f78
color_variation = 14

[sand]
kind = powder
density = 1600
slide = 0.85
color = #d8b86a
color_variation = 18

[cinder_grit]
kind = powder
density = 2400
slide = 0.45
color = #7a5a4c
color_variation = 22

[water]
kind = liquid
density = 1000
viscosity = 0.0
spread = 6
color = #3a78c8c8
color_variation = 8

[lamp_oil]
kind = liquid
density = 820
viscosity = 0.15
spread = 5
color = #9c7a2ed0
color_variation = 10

[amber_syrup]
kind = liquid
density = 1400
viscosity = 0.85
spread = 2
color = #c8661ce8
color_variation = 10

[quickmetal]
kind = liquid
density = 13500
viscosity = 0.05
spread = 5
color = #c4ccd6
color_variation = 12

[steam]
kind = gas
density = 0.6
spread = 2
color = #c8d2dc90
color_variation = 12

[murk]
kind = gas
density = 3.5
spread = 2
color = #5c7a3ca0
color_variation = 14
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
    buildPalette(air);
    materials_.push_back(air);
    byName_["air"] = kEmpty;
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
        if (cur.name == "air") { fail(sectionLine, "'air' is built in and can't be redefined"); return; }
        if (cur.kind == MaterialKind::Empty) { fail(sectionLine, "material '" + cur.name + "' needs a kind"); return; }
        if (materials_.size() >= 0xFFFF) { fail(sectionLine, "too many materials"); return; }
        cur.spread = std::clamp(cur.spread, 0, 16);
        cur.maxFall = std::clamp(cur.maxFall, 1, 16);
        cur.viscosity = std::clamp(cur.viscosity, 0.0f, 1.0f);
        cur.slide = std::clamp(cur.slide, 0.0f, 1.0f);
        cur.colorVariation = std::clamp(cur.colorVariation, 0, 128);
        if (cur.density <= 0.0f) cur.density = 0.01f;
        if (cur.label.empty()) {
            cur.label = cur.name;
            std::replace(cur.label.begin(), cur.label.end(), '_', ' ');
            cur.label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cur.label[0])));
        }
        buildPalette(cur);
        auto it = byName_.find(cur.name);
        if (it != byName_.end()) {
            cur.id = it->second;
            materials_[cur.id] = cur;
        } else {
            cur.id = static_cast<MaterialId>(materials_.size());
            byName_[cur.name] = cur.id;
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
        else if (key == "color") good = parseColor(val, cur.color);
        else if (key == "color_variation") good = parseInt(val, cur.colorVariation);
        else cur.extra[key] = std::string(val);
        if (!good) fail(lineNo, "bad value for '" + key + "': " + std::string(val));
    }
    commit();
    return ok;
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
