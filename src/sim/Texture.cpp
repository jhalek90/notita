#include "Texture.h"

#include "Rng.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace notita {

std::uint8_t terrainShade(int x, int y) {
    const std::uint64_t h = Rng::hash(static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) * 0x9E3779B1ull,
                                      static_cast<std::uint32_t>(y));
    return static_cast<std::uint8_t>(h >> 56);
}

namespace {

// ---- hashing and noise (cheap 32-bit, spawn time only) ----
std::uint32_t mix32(std::uint32_t h) {
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}
std::uint32_t hash3(int x, int y, std::uint32_t s) {
    return mix32(static_cast<std::uint32_t>(x) * 0x9E3779B1u ^ mix32(static_cast<std::uint32_t>(y) * 0x85EBCA77u + s));
}
float unit(std::uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

float vnoise(float x, float y, std::uint32_t s) {
    const float fx0 = std::floor(x), fy0 = std::floor(y);
    const int xi = static_cast<int>(fx0), yi = static_cast<int>(fy0);
    const float fx = x - fx0, fy = y - fy0;
    const float u = fx * fx * (3.0f - 2.0f * fx), v = fy * fy * (3.0f - 2.0f * fy);
    const float a = unit(hash3(xi, yi, s)), b = unit(hash3(xi + 1, yi, s));
    const float c = unit(hash3(xi, yi + 1, s)), d = unit(hash3(xi + 1, yi + 1, s));
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}

// 0..1, three octaves
float fbm(float x, float y, std::uint32_t s, int octaves = 3) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += vnoise(x, y, s + static_cast<std::uint32_t>(o) * 0x68E31DA4u) * amp;
        norm += amp;
        amp *= 0.5f;
        x = x * 2.03f + 17.1f;
        y = y * 2.03f - 9.7f;
    }
    return sum / norm;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> split(std::string_view v) {
    std::vector<std::string_view> out;
    while (true) {
        const std::size_t p = v.find(',');
        out.push_back(trim(v.substr(0, p)));
        if (p == std::string_view::npos) break;
        v.remove_prefix(p + 1);
    }
    return out;
}

bool toFloat(std::string_view v, float& out) {
    std::string tmp(v);
    char* end = nullptr;
    const float f = std::strtof(tmp.c_str(), &end);
    if (tmp.empty() || end == tmp.c_str() || *end != '\0' || !std::isfinite(f)) return false;
    out = f;
    return true;
}

bool toInt(std::string_view v, int& out) {
    auto r = std::from_chars(v.data(), v.data() + v.size(), out);
    return r.ec == std::errc() && r.ptr == v.data() + v.size();
}

bool toColor(std::string_view v, std::uint32_t& out) {
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

} // namespace

bool parseTexture(const std::unordered_map<std::string, std::string>& extra, std::uint32_t baseColor,
                  MaterialTexture& t, std::string* error) {
    t = MaterialTexture{};
    auto get = [&](const char* k) -> const std::string* {
        auto it = extra.find(k);
        return it == extra.end() ? nullptr : &it->second;
    };
    const std::string* pal = get("palette");
    const std::string* pat = get("texture");
    if (!pal && !pat) return true; // untextured: the old brightness ramp
    auto bad = [&](const std::string& msg) {
        if (error) *error = msg;
        t = MaterialTexture{};
        return false;
    };

    // palette = #rrggbb[*weight], ... (dark to light reads best: the noise runs across it)
    std::vector<std::uint32_t> cols;
    std::vector<float> weights;
    if (pal) {
        for (std::string_view e : split(*pal)) {
            float w = 1.0f;
            const std::size_t star = e.find('*');
            if (star != std::string_view::npos) {
                if (!toFloat(trim(e.substr(star + 1)), w) || w <= 0.0f) return bad("palette: bad weight in '" + std::string(e) + "'");
                e = trim(e.substr(0, star));
            }
            std::uint32_t c = 0;
            if (!toColor(e, c)) return bad("palette: bad colour '" + std::string(e) + "'");
            cols.push_back(c);
            weights.push_back(w);
        }
        if (cols.empty() || cols.size() > 16) return bad("palette needs 1 to 16 colours");
    } else {
        cols.push_back(baseColor);
        weights.push_back(1.0f);
    }
    t.colorCount = static_cast<int>(cols.size());
    float total = 0.0f;
    for (float w : weights) total += w;
    float acc = 0.0f;
    for (int i = 0; i < t.colorCount; ++i) {
        t.colors[i] = cols[i];
        acc += weights[i] / total;
        t.cumWeight[i] = acc;
    }
    t.cumWeight[t.colorCount - 1] = 1.0f;
    // 16 slots shared out by weight (largest remainder), at least one per colour
    {
        std::array<int, 16> n{};
        int used = 0;
        for (int i = 0; i < t.colorCount; ++i) {
            n[i] = std::max(1, static_cast<int>(std::floor(weights[i] / total * 16.0f)));
            used += n[i];
        }
        while (used > 16) { // only when many tiny weights got bumped to 1
            int big = 0;
            for (int i = 1; i < t.colorCount; ++i) if (n[i] > n[big]) big = i;
            --n[big]; --used;
        }
        while (used < 16) {
            int best = 0;
            float bestGap = -1e9f;
            for (int i = 0; i < t.colorCount; ++i) {
                const float gap = weights[i] / total * 16.0f - static_cast<float>(n[i]);
                if (gap > bestGap) { bestGap = gap; best = i; }
            }
            ++n[best]; ++used;
        }
        int s = 0;
        for (int i = 0; i < t.colorCount; ++i) {
            t.firstSlot[i] = static_cast<std::uint8_t>(s);
            t.slotCount[i] = static_cast<std::uint8_t>(n[i]);
            for (int k = 0; k < n[i]; ++k) t.slotColor[s++] = static_cast<std::uint8_t>(i);
        }
    }

    if (pat) {
        const std::string& p = *pat;
        if (p == "clumps") t.pattern = TexturePattern::Clumps;
        else if (p == "speckle") t.pattern = TexturePattern::Speckle;
        else if (p == "strata") t.pattern = TexturePattern::Strata;
        else if (p == "grain") t.pattern = TexturePattern::Grain;
        else if (p == "brushed") t.pattern = TexturePattern::Brushed;
        else return bad("texture: unknown pattern '" + p + "' (clumps, speckle, strata, grain, brushed)");
    }

    auto num = [&](const char* k, float& v, float lo, float hi) {
        const std::string* s = get(k);
        if (!s) return true;
        float f = 0.0f;
        if (!toFloat(trim(*s), f)) return false;
        v = std::clamp(f, lo, hi);
        return true;
    };
    if (!num("tex_scale", t.scale, 0.5f, 256.0f) || !num("tex_stretch", t.stretch, 1.0f, 64.0f) ||
        !num("tex_mix", t.mix, 0.0f, 1.0f) || !num("tex_contrast", t.contrast, 0.1f, 8.0f) ||
        !num("tex_shade", t.shadeNoise, 0.0f, 1.0f) || !num("tex_jitter", t.jitter, 0.0f, 1.0f) ||
        !num("tex_warp", t.warp, 0.0f, 256.0f) || !num("pores", t.poreChance, 0.0f, 1.0f) ||
        !num("rings", t.ringFreq, 0.0f, 4.0f))
        return bad("bad texture number (tex_scale, tex_stretch, tex_mix, tex_contrast, tex_shade, tex_jitter, tex_warp, pores, rings)");

    // "chance, colour" style lists; colours are 0-based palette indices
    auto list = [&](const char* k, std::vector<float>& v, std::size_t n) {
        const std::string* s = get(k);
        if (!s) return true;
        const auto parts = split(*s);
        if (parts.size() != n) return false;
        v.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            if (!toFloat(parts[i], v[i])) return false;
        return true;
    };
    auto color = [&](float f) { return std::clamp(static_cast<int>(f), 0, t.colorCount - 1); };
    std::vector<float> v;
    if (!list("flecks", v, 2)) return bad("flecks = chance, colour");
    if (!v.empty()) { t.fleckChance = std::clamp(v[0], 0.0f, 1.0f); t.fleckColor = color(v[1]); v.clear(); }
    if (!list("specks", v, 2)) return bad("specks = chance, colour");
    if (!v.empty()) { t.speckChance = std::clamp(v[0], 0.0f, 1.0f); t.speckColor = color(v[1]); v.clear(); }
    if (!list("pebbles", v, 3)) return bad("pebbles = chance, radius, colour");
    if (!v.empty()) { t.pebbleChance = std::clamp(v[0], 0.0f, 1.0f); t.pebbleSize = std::clamp(static_cast<int>(v[1]), 1, 8); t.pebbleColor = color(v[2]); v.clear(); }
    if (!list("panels", v, 3)) return bad("panels = width, height, amount");
    if (!v.empty()) { t.panelW = std::clamp(static_cast<int>(v[0]), 0, 1024); t.panelH = std::clamp(static_cast<int>(v[1]), 0, 1024); t.panelAmount = std::clamp(v[2], 0.0f, 1.0f); v.clear(); }
    if (!list("veins", v, 3)) return bad("veins = scale, width, colour");
    if (!v.empty()) { t.veinScale = std::clamp(v[0], 0.0f, 512.0f); t.veinWidth = std::clamp(v[1], 0.0f, 8.0f); t.veinColor = color(v[2]); v.clear(); }
    if (!list("knots", v, 3)) return bad("knots = chance, radius, colour");
    if (!v.empty()) { t.knotChance = std::clamp(v[0], 0.0f, 1.0f); t.knotSize = std::clamp(static_cast<int>(v[1]), 1, 12); t.knotColor = color(v[2]); v.clear(); }

    t.enabled = true;
    return true;
}

std::uint8_t textureShade(const MaterialTexture& t, int x, int y, std::uint32_t seed, int along) {
    if (!t.enabled) return terrainShade(x, y);
    const std::uint32_t s = mix32(seed * 0x27D4EB2Fu + 0x165667B1u);
    const std::uint32_t hp = hash3(x, y, s);           // per-pixel randomness
    const std::uint32_t hq = hash3(x, y, s ^ 0xA511E9B3u);
    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
    const float sc = t.scale;
    // along / across the beam for the directional patterns
    const float a = along ? fy : fx, c = along ? fx : fy;

    float f = 0.5f;      // colour pick, 0..1 before contrast
    float b = 0.0f;      // brightness, about -1..1
    switch (t.pattern) {
    case TexturePattern::Clumps:
        f = fbm(fx / sc, fy / sc, s);
        b = (fbm(fx / (sc * 0.5f), fy / (sc * 0.5f), s + 1u, 2) - 0.5f) * 2.0f;
        break;
    case TexturePattern::Speckle:
        f = unit(hq);
        b = (vnoise(fx / sc, fy / sc, s + 1u) - 0.5f) * 2.0f;
        break;
    case TexturePattern::Strata: {
        const float w = t.warp * (fbm(fx / (sc * t.stretch), fy / (sc * 2.0f), s + 2u, 2) - 0.5f) * 2.0f;
        const float band = (fy + w) / sc;
        f = 0.7f * vnoise(band, 0.37f, s) + 0.3f * fbm(fx / sc, fy / sc, s + 3u, 2);
        b = (vnoise(band * 2.3f, 5.1f, s + 4u) - 0.5f) * 1.4f + (vnoise(fx / (sc * t.stretch), band, s + 5u) - 0.5f) * 0.8f;
        break;
    }
    case TexturePattern::Grain:
    case TexturePattern::Brushed: {
        const float streak = fbm(a / (sc * t.stretch), c / sc, s, 2);
        f = streak;
        b = (vnoise(a / (sc * t.stretch * 0.5f), c / (sc * 0.4f), s + 1u) - 0.5f) * 2.0f;
        if (t.pattern == TexturePattern::Brushed) {
            // a soft sheen band running across the plate
            b = b * 0.6f + 0.5f * std::sin((a * 0.6f + c) / (sc * 6.0f) + 1.3f);
        }
        break;
    }
    }

    // contrast + per-pixel mix, then the weighted pick
    // (fbm has a spread of about 0.12; x 1.5 x contrast 1.6 stretches it to roughly
    // even over 0..1, so the palette weights hold; speckle is uniform already)
    float pick = t.pattern == TexturePattern::Speckle ? f : std::clamp(0.5f + (f - 0.5f) * t.contrast * 1.5f, 0.0f, 0.9999f);
    pick = pick + (unit(hq) - pick) * t.mix;
    int col = 0;
    while (col < t.colorCount - 1 && pick >= t.cumWeight[col]) ++col;
    float level = b * t.shadeNoise + (unit(hp) - 0.5f) * 2.0f * t.jitter;

    // growth rings (wood): dark lines across the grain, bent around knots
    if (t.ringFreq > 0.0f || t.knotChance > 0.0f) {
        float bend = 0.0f;
        if (t.knotChance > 0.0f) {
            const int ks = t.knotSize;
            const int ga = ks * 10, gc = ks * 4;
            const int ai = static_cast<int>(std::floor(a / static_cast<float>(ga)));
            const int ci = static_cast<int>(std::floor(c / static_cast<float>(gc)));
            for (int di = -1; di <= 1; ++di)
                for (int dj = -1; dj <= 1; ++dj) {
                    const std::uint32_t kh = hash3(ai + di, ci + dj, s ^ 0x3C6EF372u);
                    if (unit(kh) >= t.knotChance) continue;
                    const float ka = (static_cast<float>(ai + di) + 0.2f + 0.6f * unit(mix32(kh))) * static_cast<float>(ga);
                    const float kc = (static_cast<float>(ci + dj) + 0.2f + 0.6f * unit(mix32(kh ^ 0x5bd1e995u))) * static_cast<float>(gc);
                    const float da = (a - ka) / 1.7f, dc = c - kc;
                    const float d = std::sqrt(da * da + dc * dc);
                    const float r = static_cast<float>(ks) * (0.7f + 0.6f * unit(mix32(kh ^ 0x1b873593u)));
                    if (d < r) {
                        col = t.knotColor;
                        level = -0.2f - 0.6f * (1.0f - d / r) + (unit(hp) - 0.5f) * 0.3f;
                        return makeShade(t.firstSlot[col] + static_cast<int>(hp % t.slotCount[col]),
                                         static_cast<int>(std::lround(7.5f + std::clamp(level, -1.0f, 1.0f) * 7.5f)));
                    }
                    bend += std::max(0.0f, 1.0f - d / (r * 3.0f)) * 3.0f * (dc >= 0.0f ? 1.0f : -1.0f);
                }
        }
        if (t.ringFreq > 0.0f) {
            const float phase = (c + bend) * t.ringFreq + 1.2f * vnoise(a / (sc * t.stretch * 2.0f), c / (sc * 2.0f), s + 6u);
            const float ring = phase - std::floor(phase);
            if (ring < 0.22f) level -= 0.55f;
        }
    }

    // stone veins: thin lines along a contour of warped noise, about veinWidth px
    // wide everywhere (distance to the contour = offset / gradient), broken up by
    // a second noise so they wander and stop instead of closing into rings
    if (t.veinScale > 0.0f && t.veinWidth > 0.0f &&
        fbm(fx / (t.veinScale * 1.5f), fy / (t.veinScale * 1.5f), s + 9u, 2) > 0.47f) {
        auto field = [&](float px, float py) {
            const float w = t.warp > 0.0f ? (fbm(px / (t.veinScale * 0.7f), py / (t.veinScale * 0.7f), s + 7u, 2) - 0.5f) * t.warp : 0.0f;
            return fbm((px + w) / t.veinScale, (py - w) / t.veinScale, s + 8u, 2);
        };
        const float v0 = field(fx, fy);
        const float gx = field(fx + 1.0f, fy) - v0, gy = field(fx, fy + 1.0f) - v0;
        const float grad = std::sqrt(gx * gx + gy * gy);
        if (std::fabs(v0 - 0.5f) < t.veinWidth * 0.5f * std::max(grad, 1e-5f)) {
            col = t.veinColor;
            level = level * 0.4f + 0.15f;
        }
    }

    // pebbles: small shaded discs on a jittered grid
    if (t.pebbleChance > 0.0f) {
        const int g = t.pebbleSize * 4 + 2;
        const int gx = static_cast<int>(std::floor(fx / static_cast<float>(g))), gy = static_cast<int>(std::floor(fy / static_cast<float>(g)));
        for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di) {
                const std::uint32_t ph = hash3(gx + di, gy + dj, s ^ 0x9E3779B9u);
                if (unit(ph) >= t.pebbleChance) continue;
                const float px = (static_cast<float>(gx + di) + 0.25f + 0.5f * unit(mix32(ph))) * static_cast<float>(g);
                const float py = (static_cast<float>(gy + dj) + 0.25f + 0.5f * unit(mix32(ph ^ 0x7F4A7C15u))) * static_cast<float>(g);
                const float r = static_cast<float>(t.pebbleSize) * (0.6f + 0.6f * unit(mix32(ph ^ 0x2545F491u)));
                const float dx = fx + 0.5f - px, dy = fy + 0.5f - py;
                if (dx * dx + dy * dy <= r * r) {
                    col = t.pebbleColor;
                    // lit from the upper left
                    level = std::clamp(-(dx + dy) / (r * 1.5f), -0.8f, 0.8f) + (unit(hp) - 0.5f) * 0.25f;
                }
            }
    }

    const float u3 = unit(mix32(hp ^ 0xC2B2AE35u));
    if (u3 < t.speckChance) col = t.speckColor;
    else if (u3 < t.speckChance + t.fleckChance) col = t.fleckColor;
    if (t.poreChance > 0.0f && unit(mix32(hq ^ 0x27D4EB2Fu)) < t.poreChance) level = -0.85f - 0.15f * unit(hp);

    if (t.panelW > 0 && t.panelH > 0 && t.panelAmount > 0.0f) {
        const int bx = static_cast<int>(std::floor(fx / static_cast<float>(t.panelW)));
        const int by = static_cast<int>(std::floor(fy / static_cast<float>(t.panelH)));
        level += (unit(hash3(bx, by, s ^ 0x61C88647u)) - 0.5f) * 2.0f * t.panelAmount;
    }

    const int slot = t.firstSlot[col] + static_cast<int>(hp % t.slotCount[col]);
    return makeShade(slot, static_cast<int>(std::lround(7.5f + std::clamp(level, -1.0f, 1.0f) * 7.5f)));
}

void buildTexturePalette(const MaterialTexture& t, int colorVariation, std::array<std::uint32_t, 256>& palette) {
    for (int s = 0; s < 256; ++s) {
        const std::uint32_t base = t.colors[t.slotColor[shadeSlot(static_cast<std::uint8_t>(s))]];
        const int r = base & 0xFF, g = (base >> 8) & 0xFF, b = (base >> 16) & 0xFF;
        const int off = static_cast<int>(std::lround((static_cast<float>(shadeLevel(static_cast<std::uint8_t>(s))) - 7.5f) / 7.5f *
                                                     static_cast<float>(colorVariation)));
        auto ch = [](int v) { return static_cast<std::uint32_t>(std::clamp(v, 0, 255)); };
        // blue moves a little less than red/green: darker shades lean cool, lighter warm (as the plain ramp)
        palette[s] = ch(r + off) | (ch(g + off) << 8) | (ch(b + off - off / 4) << 16) | (base & 0xFF000000u);
    }
}

} // namespace notita
