#pragma once
// Per-pixel material textures (#481). A textured material has a small palette
// (data/materials.txt `palette = ...`) and layered world-space noise that is
// sampled once, when a pixel is created or placed. The result is written into
// Cell::shade, so the look sticks to the pixel when it moves instead of
// shimmering in world space, and saves need no new field.
//
// Shade layout for textured materials: the high nibble is a palette slot
// (16 slots, shared out among the colours by their weights), the low nibble a
// brightness level (16 steps over +- color_variation). Any byte is a valid
// shade, so random shades (old saves, sparks) still give a weighted grain mix.
// Untextured materials keep the old meaning (a 256-step brightness ramp) and
// get terrainShade(x, y) from shadeAt, exactly like before.
//
// Pure functions of (parameters, x, y): WorldGen stays a function of the seed
// and position, and ChunkCodec can store shades as a delta to shadeAt.
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace notita {

enum class TexturePattern : std::uint8_t {
    Clumps,  // soft blotches (dirt, ash)
    Speckle, // every pixel picks on its own (sand grains)
    Strata,  // wavy horizontal bands (stone, basalt)
    Grain,   // streaks along the beam with growth rings and knots (wood)
    Brushed, // long faint streaks along x (metals)
};

struct MaterialTexture {
    bool enabled = false;
    std::uint32_t seed = 0; // from the material name (stable when ids shift), set by the registry
    TexturePattern pattern = TexturePattern::Clumps;
    int colorCount = 0;
    std::array<std::uint32_t, 16> colors{};     // RGBA8 in memory order
    std::array<std::uint8_t, 16> slotColor{};   // slot -> colour index
    std::array<std::uint8_t, 16> firstSlot{};   // colour -> its first slot
    std::array<std::uint8_t, 16> slotCount{};   // colour -> how many slots
    std::array<float, 16> cumWeight{};          // colour pick thresholds over 0..1

    float scale = 6.0f;      // feature size in px (tex_scale)
    float stretch = 6.0f;    // grain / brushed / strata: how much longer along than across (tex_stretch)
    float mix = 0.25f;       // per-pixel randomness in the colour pick, 0..1 (tex_mix)
    float contrast = 1.6f;   // spreads the noise over the palette (tex_contrast)
    float shadeNoise = 0.6f; // brightness from noise (tex_shade), 0..1
    float jitter = 0.35f;    // per-pixel brightness jitter (tex_jitter), 0..1
    float warp = 0.0f;       // strata / veins: domain warp in px (tex_warp)

    // layers on top, all off by default; colour indices are 0-based into the palette
    float fleckChance = 0.0f; int fleckColor = 0;              // flecks = chance, colour
    float speckChance = 0.0f; int speckColor = 0;              // specks = chance, colour
    float pebbleChance = 0.0f; int pebbleSize = 2; int pebbleColor = 0; // pebbles = chance, radius, colour
    float poreChance = 0.0f;                                   // pores = chance (dark pits)
    int panelW = 0, panelH = 0; float panelAmount = 0.0f;      // panels = width, height, amount
    float veinScale = 0.0f, veinWidth = 0.0f; int veinColor = 0; // veins = scale, width (px), colour
    float knotChance = 0.0f; int knotSize = 4; int knotColor = 0; // knots = chance, radius, colour
    float ringFreq = 0.0f;                                     // rings = growth rings per px across the grain
};

// Parses the texture keys from a material's unknown-key map. Returns false
// (and an error text) on a bad value; untextured materials leave enabled false.
// baseColor is the single palette colour when only `texture` is given.
bool parseTexture(const std::unordered_map<std::string, std::string>& extra, std::uint32_t baseColor,
                  MaterialTexture& out, std::string* error);

// The plain per-position hash shade (untextured materials, old saves).
std::uint8_t terrainShade(int x, int y);

// The shade a pixel at (x, y) gets. `along` is the beam direction for the
// grain patterns: 0 = along x (default), 1 = along y.
std::uint8_t textureShade(const MaterialTexture& t, int x, int y, int along = 0);

// FNV-1a of a material name, the texture seed.
std::uint32_t textureSeed(const std::string& name);

// Shade byte helpers for textured materials.
constexpr std::uint8_t shadeSlot(std::uint8_t s) { return static_cast<std::uint8_t>(s >> 4); }
constexpr std::uint8_t shadeLevel(std::uint8_t s) { return static_cast<std::uint8_t>(s & 15); }
constexpr std::uint8_t makeShade(int slot, int level) {
    return static_cast<std::uint8_t>(((slot & 15) << 4) | (level < 0 ? 0 : level > 15 ? 15 : level));
}

// Fills the 256-entry palette of a textured material (color_variation = the
// brightness spread over the 16 levels).
void buildTexturePalette(const MaterialTexture& t, int colorVariation, std::array<std::uint32_t, 256>& palette);

} // namespace notita
