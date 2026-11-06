#include "WorldGen.h"

#include "Bodies.h"
#include "ChunkCodec.h"
#include "Rng.h"

#include <algorithm>
#include <cmath>

namespace notita {

const char* biomeName(Biome b) {
    switch (b) {
    case Biome::Meadow: return "meadow";
    case Biome::Dunes: return "dunes";
    case Biome::Ashlands: return "ashlands";
    default: return "?";
    }
}

namespace {
constexpr std::uint64_t kSurface = 1, kHills = 2, kBiome = 3, kWater = 4, kLayer = 5, kIron = 6, kBedrock = 7,
                        kTunnel = 8, kCavern = 9, kLava = 10, kOil = 11, kGas = 12, kBasalt = 13;

float smooth(float t) { return t * t * (3.0f - 2.0f * t); }
} // namespace

WorldGen::WorldGen(const MaterialRegistry& mats, std::uint64_t seed, WorldGenParams params)
    : mats_(mats), seed_(seed), params_(params) {
    auto id = [&](const char* name, MaterialId fallback) {
        const MaterialId m = mats.idOf(name);
        return m != kEmpty ? m : fallback;
    };
    slate_ = id("slate", kEmpty);
    if (slate_ == kEmpty && mats.size() > 1) slate_ = 1;
    basalt_ = id("basalt", slate_);
    sand_ = id("sand", slate_);
    grit_ = id("cinder_grit", sand_);
    loam_ = id("loam", slate_);
    turf_ = id("turf", loam_);
    iron_ = id("iron", slate_);
    bedrock_ = id("bedrock", slate_);
    water_ = id("water", kEmpty);
    magma_ = id("magma", kEmpty);
    hydrogen_ = id("hydrogen", kEmpty);
    murk_ = id("murk", hydrogen_);
    oil_ = id("lamp_oil", water_);
}

float WorldGen::noise1(float x, std::uint64_t salt) const {
    const float fx = std::floor(x);
    const std::int64_t i = static_cast<std::int64_t>(fx);
    const std::uint64_t s = Rng::hash(seed_, salt);
    auto at = [&](std::int64_t k) {
        return static_cast<float>(Rng::hash(s, static_cast<std::uint64_t>(k)) >> 40) * (2.0f / 16777216.0f) - 1.0f;
    };
    const float t = smooth(x - fx);
    return at(i) + (at(i + 1) - at(i)) * t;
}

float WorldGen::noise2(float x, float y, std::uint64_t salt) const {
    const float fx = std::floor(x), fy = std::floor(y);
    const std::int64_t i = static_cast<std::int64_t>(fx), j = static_cast<std::int64_t>(fy);
    const std::uint64_t s = Rng::hash(seed_, salt);
    auto at = [&](std::int64_t a, std::int64_t b) {
        const std::uint64_t h = Rng::hash(Rng::hash(s, static_cast<std::uint64_t>(a)), static_cast<std::uint64_t>(b));
        return static_cast<float>(h >> 40) * (2.0f / 16777216.0f) - 1.0f;
    };
    const float tx = smooth(x - fx), ty = smooth(y - fy);
    const float a = at(i, j) + (at(i + 1, j) - at(i, j)) * tx;
    const float b = at(i, j + 1) + (at(i + 1, j + 1) - at(i, j + 1)) * tx;
    return a + (b - a) * ty;
}

float WorldGen::fbm1(float x, int octaves, std::uint64_t salt) const {
    float sum = 0, amp = 1, norm = 0;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * noise1(x, salt * 31 + static_cast<std::uint64_t>(o));
        norm += amp;
        amp *= 0.5f;
        x *= 2.03f;
    }
    return sum / norm;
}

float WorldGen::fbm2(float x, float y, int octaves, std::uint64_t salt) const {
    float sum = 0, amp = 1, norm = 0;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * noise2(x, y, salt * 31 + static_cast<std::uint64_t>(o));
        norm += amp;
        amp *= 0.5f;
        x *= 2.03f;
        y *= 1.97f;
    }
    return sum / norm;
}

WorldGen::Column WorldGen::column(int x) const {
    const float fx = static_cast<float>(x);
    Column c;
    const float b = fbm1(fx / 1400.0f + 7.3f, 2, kBiome);
    c.biome = b < -0.18f ? Biome::Dunes : (b > 0.22f ? Biome::Ashlands : Biome::Meadow);
    c.blend = b;
    float s = fbm1(fx / 420.0f, 3, kSurface) * params_.surfaceAmp + fbm1(fx / 70.0f, 2, kHills) * params_.hillAmp;
    if (c.biome == Biome::Dunes) s += 18.0f * std::sin(fx / 45.0f) * 0.5f; // dune ripples
    c.surface = static_cast<int>(std::floor(s));
    if (overlay_) c.surface = overlay_->surface(x, c.surface);
    c.water = c.surface + params_.waterTable + static_cast<int>(fbm1(fx / 650.0f, 2, kWater) * params_.waterVariation);
    return c;
}

int WorldGen::surfaceY(int x) const { return column(x).surface; }
Biome WorldGen::biome(int x) const { return column(x).biome; }
int WorldGen::waterTableY(int x) const { return column(x).water; }

Cell WorldGen::make(MaterialId m, int x, int y) const {
    Cell c = mats_[m].spawn(terrainShade(x, y));
    if (mats_[m].kind == MaterialKind::Solid) c.aux = kAuxPinned;
    return c;
}

Cell WorldGen::cellAt(int x, int y) const {
    if (Cell o; overlay_ && overlay_->cellAt(x, y, o)) return o;
    return cellIn(column(x), x, y);
}

Cell WorldGen::cellIn(const Column& col, int x, int y) const {
    const int d = y - col.surface;
    if (d < 0) return Cell{};
    const float fx = static_cast<float>(x), fy = static_cast<float>(y);

    // caves first (they cut through every layer below the minimum depth)
    const int minDepth = col.biome == Biome::Meadow ? 14 : (col.biome == Biome::Ashlands ? 30 : params_.caveDepth);
    if (d > minDepth) {
        const float taper = std::clamp(static_cast<float>(d - minDepth) / 40.0f, 0.0f, 1.0f);
        const float t = fbm2(fx / 150.0f, fy / 80.0f, 3, kTunnel);
        const float cav = fbm2(fx / 260.0f, fy / 170.0f, 3, kCavern);
        if (std::abs(t) < params_.tunnelWidth * taper || cav * taper > params_.cavernLevel) {
            const float lava = fbm2(fx / 380.0f, fy / 380.0f, 2, kLava);
            const bool deep = d > params_.lavaDepth || (col.biome == Biome::Ashlands && d > 90);
            if (magma_ != kEmpty && deep && lava > 0.12f) return make(magma_, x, y);
            if (water_ != kEmpty && y >= col.water) return make(water_, x, y);
            if (oil_ != kEmpty && y >= col.water - 26 && fbm2(fx / 300.0f, fy / 300.0f, 2, kOil) > 0.3f) return make(oil_, x, y);
            if (hydrogen_ != kEmpty && fbm2(fx / 220.0f, fy / 220.0f, 2, kGas) > 0.38f &&
                (Rng::hash(seed_ ^ kGas, (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
                                             static_cast<std::uint32_t>(y)) & 7) == 0)
                return make(d < 320 ? hydrogen_ : murk_, x, y); // sparse: every gas pixel is real mass
            return Cell{};
        }
    }

    // surface layers by biome
    const float layer = fbm1(fx / 60.0f, 2, kLayer);
    switch (col.biome) {
    case Biome::Meadow:
        if (d < 3) return make(turf_, x, y);
        if (d < 14 + static_cast<int>(layer * 6.0f)) return make(loam_, x, y);
        break;
    case Biome::Dunes:
        if (d < 34 + static_cast<int>(layer * 8.0f)) return make(sand_, x, y);
        break;
    case Biome::Ashlands:
        if (d < 5) return make(grit_, x, y);
        if (d < 70 + static_cast<int>(layer * 20.0f)) return make(basalt_, x, y);
        break;
    default: break;
    }

    // rock
    if (d > 300 && fbm2(fx / 90.0f, fy / 90.0f, 2, kBedrock) > 0.62f) return make(bedrock_, x, y);
    if (d > 30 && fbm2(fx / 34.0f, fy / 34.0f, 2, kIron) > 0.5f) return make(iron_, x, y);
    if (d > params_.basaltDepth + static_cast<int>(fbm1(fx / 200.0f, 2, kBasalt) * 80.0f)) return make(basalt_, x, y);
    return make(slate_, x, y);
}

void WorldGen::generate(Chunk& chunk) const {
    const int ox = chunk.originX(), oy = chunk.originY();
    for (int lx = 0; lx < kChunkSize; ++lx) {
        const int x = ox + lx;
        const Column col = column(x);
        for (int ly = 0; ly < kChunkSize; ++ly) {
            const int y = oy + ly;
            if (Cell o; overlay_ && overlay_->cellAt(x, y, o)) chunk.at(lx, ly) = o;
            else chunk.at(lx, ly) = y < col.surface ? Cell{} : cellIn(col, x, y);
        }
    }
}

} // namespace notita
