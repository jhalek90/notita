// #481: per-pixel material textures (src/sim/Texture.*): parsing, the shade
// a pixel gets, and how the chunk codec stores shades against the texture.
#include "test.h"

#include "sim/ChunkCodec.h"
#include "sim/Material.h"
#include "sim/Scenes.h"
#include "sim/Simulator.h"
#include "sim/Texture.h"
#include "sim/ThreadPool.h"
#include "sim/World.h"
#include "sim/WorldGen.h"

#include <array>
#include <chrono>
#include <cstring>

using namespace notita;

namespace {

// own table, so retuning data/materials.txt doesn't break these
const char* kTexMaterials = R"(
[bedrock]
kind = solid
density = 5000
anchor = true
color = #202020
[slate]
kind = solid
density = 2700
color = #707070
color_variation = 12
palette = #505050*1, #707070*2, #909090*1, #ffffff*0.02 ; the last one only as veins
texture = strata
tex_scale = 6
tex_warp = 8
veins = 40, 1.0, 3
[loam]
kind = solid
density = 1600
color = #6a4a2f
color_variation = 16
palette = #4a3220*2, #6a4a2f*4, #7b5a3a*2, #2b1d13*0.02, #857d70*0.02
flecks = 0.1, 3
pebbles = 0.2, 2, 4
[sand]
kind = powder
density = 1600
color = #d8b86a
color_variation = 18
palette = #c0a060, #d8b86a*3, #e8d0a0
texture = speckle
[timber]
kind = solid
density = 600
color = #7b5232
palette = #6c4729, #7b5232, #3a2414*0.02
texture = grain
rings = 0.3
knots = 0.2, 3, 2
[iron]
kind = solid
density = 7800
color = #8a8f96
color_variation = 10
texture = brushed
[water]
kind = liquid
density = 1000
color = #3060c0
)";

MaterialRegistry texMats() {
    MaterialRegistry m;
    std::vector<std::string> errors;
    m.loadFromString(kTexMaterials, &errors);
    for (auto& e : errors) std::printf("  materials: %s\n", e.c_str());
    CHECK(errors.empty());
    return m;
}

// how many pixels of a 256x256 area pick each palette colour
std::array<int, 16> colourCounts(const Material& m, int along = 0) {
    std::array<int, 16> n{};
    for (int y = -128; y < 128; ++y)
        for (int x = -128; x < 128; ++x) ++n[m.texture.slotColor[shadeSlot(m.shadeAt(x, y, along))]];
    return n;
}

} // namespace

TEST_CASE(texture_palette_parses_with_weights_and_slots) {
    MaterialRegistry mats = texMats();
    const Material& slate = *mats.find("slate");
    CHECK(slate.texture.enabled);
    CHECK_EQ(slate.texture.colorCount, 4);         // the '#' after a comma is a colour, not a comment
    CHECK(slate.texture.pattern == TexturePattern::Strata);
    CHECK_EQ(slate.texture.slotCount[1], 7);       // weight 2 of 4.02: 7.96 slots, the remainders go to 0 and 2
    CHECK_EQ(slate.texture.slotCount[0], 4);
    CHECK_EQ(slate.texture.slotCount[3], 1);       // tiny weights still get one slot
    int total = 0;
    for (int i = 0; i < slate.texture.colorCount; ++i) total += slate.texture.slotCount[i];
    CHECK_EQ(total, 16);
    // the palette entries follow the slots: slot of colour 3 at the middle level is white-ish
    const std::uint32_t white = slate.palette[makeShade(slate.texture.firstSlot[3], 8)];
    CHECK((white & 0xFF) > 0xF0);
    // brightness levels run dark to light over +- color_variation
    const std::uint32_t lo = slate.palette[makeShade(slate.texture.firstSlot[1], 0)];
    const std::uint32_t hi = slate.palette[makeShade(slate.texture.firstSlot[1], 15)];
    CHECK_EQ(static_cast<int>(lo & 0xFF), 0x70 - 12);
    CHECK_EQ(static_cast<int>(hi & 0xFF), 0x70 + 12);
    // texture without a palette: one colour, brightness only
    const Material& iron = *mats.find("iron");
    CHECK(iron.texture.enabled);
    CHECK_EQ(iron.texture.colorCount, 1);
    CHECK(!mats.find("water")->texture.enabled);
}

TEST_CASE(texture_bad_keys_are_reported) {
    MaterialRegistry m;
    std::vector<std::string> errors;
    m.loadFromString("[a]\nkind = solid\npalette = #112233, nope\n[b]\nkind = solid\ntexture = plaid\n[c]\nkind = solid\n"
                     "palette = #112233\nflecks = 0.1\n",
                     &errors);
    CHECK_EQ(errors.size(), 3u);
    // a material with a bad texture still loads, untextured
    CHECK(m.find("a") && !m.find("a")->texture.enabled);
}

TEST_CASE(texture_shade_is_a_pure_function_of_position) {
    MaterialRegistry mats = texMats();
    MaterialRegistry again = texMats();
    for (const char* n : {"slate", "loam", "sand", "timber", "iron"}) {
        const Material& a = *mats.find(n);
        const Material& b = *again.find(n);
        for (int i = 0; i < 2000; ++i) {
            const int x = i * 37 - 30000, y = (i * 91) % 4000 - 2000;
            CHECK_EQ(a.shadeAt(x, y), b.shadeAt(x, y));
            CHECK_EQ(a.shadeAt(x, y), a.shadeAt(x, y));
        }
    }
    // a mod that adds a material first shifts every id, but not the textures
    MaterialRegistry shifted;
    shifted.loadFromString(std::string("[moss]\nkind = solid\ncolor = #00ff00\n") + kTexMaterials);
    CHECK(shifted.idOf("loam") != mats.idOf("loam"));
    for (int i = 0; i < 500; ++i) CHECK_EQ(shifted.find("loam")->shadeAt(i, i * 3), mats.find("loam")->shadeAt(i, i * 3));
    // untextured materials keep the old per-position hash
    const Material& bedrock = *mats.find("bedrock");
    CHECK(!bedrock.texture.enabled);
    for (int i = 0; i < 100; ++i) CHECK_EQ(bedrock.shadeAt(i * 13, -i * 7), terrainShade(i * 13, -i * 7));
}

TEST_CASE(texture_uses_the_palette_by_weight_and_layers) {
    MaterialRegistry mats = texMats();
    // sand: every grain picks on its own, weights 1:3:1
    const auto sand = colourCounts(*mats.find("sand"));
    CHECK(sand[0] > 3000 && sand[2] > 3000);
    CHECK(sand[1] > sand[0] * 2 && sand[1] > sand[2] * 2);
    // loam: the main colours, plus flecks (colour 3) and pebbles (colour 4) from the layers
    const auto loam = colourCounts(*mats.find("loam"));
    CHECK(loam[1] > loam[0] && loam[1] > loam[2]);
    CHECK(loam[3] > 65536 / 20 && loam[3] < 65536 / 5); // ~10 % flecks
    CHECK(loam[4] > 200);
    // slate: veins are thin and rare, but there
    const auto slate = colourCounts(*mats.find("slate"));
    CHECK(slate[3] > 30 && slate[3] < 65536 / 10);
    // timber: knots appear, and the grain follows the beam: along x it changes
    // slowly along x, along y slowly along y
    const Material& timber = *mats.find("timber");
    CHECK(colourCounts(timber)[2] > 50);
    int sameX = 0, sameY = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            sameX += shadeLevel(timber.shadeAt(x, y, 0)) / 4 == shadeLevel(timber.shadeAt(x + 1, y, 0)) / 4;
            sameY += shadeLevel(timber.shadeAt(x, y, 1)) / 4 == shadeLevel(timber.shadeAt(x, y + 1, 1)) / 4;
        }
    int acrossX = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) acrossX += shadeLevel(timber.shadeAt(x, y, 0)) / 4 == shadeLevel(timber.shadeAt(x, y + 1, 0)) / 4;
    CHECK(sameX > acrossX);
    CHECK(sameY > acrossX);
    // brightness uses the whole range somewhere
    std::array<int, 16> levels{};
    for (int i = 0; i < 4096; ++i) ++levels[shadeLevel(mats.find("iron")->shadeAt(i % 64, i / 64))];
    int used = 0;
    for (int l : levels) used += l > 0;
    CHECK(used >= 8);
}

TEST_CASE(texture_codec_stores_shades_against_the_texture) {
    MaterialRegistry mats = texMats();
    WorldGen gen(mats, 7);
    std::size_t plain = 0, textured = 0;
    for (int cy = 0; cy < 4; ++cy)
        for (int cx = -2; cx < 2; ++cx) {
            Chunk c, back, old;
            c.coord = back.coord = old.coord = {cx, cy};
            gen.generate(c);
            const auto blob = encodeCells(c, &mats);       // format 2
            const auto blob1 = encodeCells(c);              // format 1 (old saves)
            textured += blob.size();
            plain += blob1.size();
            CHECK(decodeCells(blob, back, nullptr, &mats));
            CHECK(std::memcmp(c.cells, back.cells, sizeof c.cells) == 0);
            CHECK(decodeCells(blob1, old));                  // format 1 still reads exactly
            CHECK(std::memcmp(c.cells, old.cells, sizeof c.cells) == 0);
            // a moved pixel keeps its shade through the codec
            std::swap(c.cells[5], c.cells[4000]);
            CHECK(decodeCells(encodeCells(c, &mats), back, nullptr, &mats));
            CHECK(std::memcmp(c.cells, back.cells, sizeof c.cells) == 0);
        }
    std::printf("  textured terrain: %zu bytes per chunk against the texture, %zu against the hash\n", textured / 16, plain / 16);
    CHECK(textured < plain);
}

TEST_CASE(texture_old_saves_get_textured_terrain) {
    // a pre-#481 chunk: terrain with the hash shade, plus one pixel someone moved
    MaterialRegistry mats = texMats();
    const MaterialId slate = mats.idOf("slate");
    Chunk c, back;
    c.coord = back.coord = {3, -2};
    for (int i = 0; i < kChunkCells; ++i) {
        const int x = c.originX() + (i & kChunkMask), y = c.originY() + (i >> kChunkShift);
        c.cells[i] = mats[slate].spawn(terrainShade(x, y));
    }
    c.cells[77].shade = static_cast<std::uint8_t>(terrainShade(c.originX() + 77, c.originY()) + 5);
    const auto blob = encodeCells(c); // format 1
    CHECK(decodeCells(blob, back, nullptr, &mats));
    CHECK_EQ(back.cells[10].shade, mats[slate].shadeAt(c.originX() + 10, c.originY()));
    CHECK_EQ(back.cells[3000].shade, mats[slate].shadeAt(c.originX() + (3000 & kChunkMask), c.originY() + (3000 >> kChunkShift)));
    CHECK_EQ(back.cells[77].shade, c.cells[77].shade); // a moved pixel keeps the shade it had
    // and the current format, read without a registry, still gives valid cells
    CHECK(decodeCells(encodeCells(back, &mats), c));
    CHECK_EQ(c.cells[500].material, slate);
}

TEST_CASE(texture_game_data_is_valid) {
    // the real data/materials.txt (run from the repo root)
    MaterialRegistry mats;
    std::vector<std::string> errors;
    if (!mats.loadFromFile("data/materials.txt", &errors) && !mats.loadFromFile("../data/materials.txt", &errors)) {
        std::printf("  data/materials.txt not found, skipped\n");
        return;
    }
    for (auto& e : errors) std::printf("  materials: %s\n", e.c_str());
    CHECK(errors.empty());
    for (const char* n : {"loam", "concrete", "slate", "timber", "sand", "iron", "ash"}) {
        const Material* m = mats.find(n);
        CHECK(m && m->texture.enabled);
        if (!m) continue;
        // what a pixel costs at spawn / in the chunk codec (printed, not checked)
        unsigned sum = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 65536; ++i) sum += m->shadeAt((i & 255) - 128, (i >> 8) - 128);
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / 65536.0;
        std::printf("  %-9s %5.0f ns per pixel (%u)\n", n, ns, sum & 1);
    }
}

TEST_CASE(texture_state_change_products_take_their_texture) {
    // a liquid that sets into textured rock, and wood that burns down to textured char;
    // both products are solids, so they stay where they were made
    MaterialRegistry mats;
    std::vector<std::string> errors;
    mats.loadFromString(R"(
[wall]
kind = solid
density = 3000
color = #404040
[melt]
kind = liquid
density = 2500
spread = 4
freeze_point = 0
freezes_into = rock
color = #c05020
[rock]
kind = solid
density = 2500
melt_point = 900
melts_into = melt
color = #707070
color_variation = 12
palette = #505050, #707070*2, #909090
texture = strata
[wood]
kind = solid
density = 600
ignite_point = 300
burn_time = 20
burns_into = smoke
burn_residue = char
residue_chance = 1
color = #7b5232
[char]
kind = solid
density = 600
color = #302820
color_variation = 14
palette = #201810*2, #302820*3, #4a4038
flecks = 0.05, 2
[smoke]
kind = gas
density = 0.9
spread = 2
color = #404040
[flame]
kind = special
density = 0.3
spawn_temp = 950
life_min = 10
life_max = 20
color = #ff9a2a
)", &errors);
    for (auto& e : errors) std::printf("  materials: %s\n", e.c_str());
    CHECK(errors.empty());
    const MaterialId rock = mats.idOf("rock"), chr = mats.idOf("char");
    World world(0, 0, 1, 1);
    ThreadPool pool(2);
    Simulator sim(world, mats, &pool, 5);
    sim.params().ambientRate = 0.0f;
    Rng rng(3);
    // a cold box of melt (random shades, as a brush or an old save would leave them)
    scenes::fillRect(world, mats.idOf("wall"), 4, 50, 30, 50, rng);
    scenes::fillRect(world, mats.idOf("wall"), 4, 30, 4, 49, rng);
    scenes::fillRect(world, mats.idOf("wall"), 30, 30, 30, 49, rng);
    scenes::fillRect(world, mats.idOf("melt"), 5, 40, 29, 49, rng, -30.0f);
    // a hot wood beam on a wall with air above it
    scenes::fillRect(world, mats.idOf("wall"), 36, 50, 60, 50, rng);
    scenes::fillRect(world, mats.idOf("wood"), 36, 48, 60, 49, rng, 400.0f);
    for (int t = 0; t < 400; ++t) sim.tick();
    std::size_t rocks = 0, chars = 0, rockMatch = 0, charMatch = 0;
    for (int y = 0; y < kChunkSize; ++y)
        for (int x = 0; x < kChunkSize; ++x) {
            const Cell& c = *world.cell(x, y);
            if (c.material == rock) { ++rocks; rockMatch += c.shade == mats[rock].shadeAt(x, y); }
            if (c.material == chr) { ++chars; charMatch += c.shade == mats[chr].shadeAt(x, y); }
        }
    CHECK_EQ(rocks, std::size_t{25 * 10});
    CHECK_EQ(rockMatch, rocks);
    CHECK(chars > 0);
    CHECK_EQ(world.count(mats.idOf("wood")), std::size_t{0});
    CHECK_EQ(charMatch, chars);
}

TEST_CASE(texture_scene_builders_texture_their_pixels) {
    MaterialRegistry mats;
    std::vector<std::string> errors;
    if (!mats.loadFromFile("data/materials.txt", &errors) && !mats.loadFromFile("../data/materials.txt", &errors)) {
        std::printf("  data/materials.txt not found, skipped\n");
        return;
    }
    CHECK(errors.empty());
    World a(0, 0, 10, 6), b(0, 0, 10, 6);
    scenes::quarry(a, mats, 77);
    scenes::quarry(b, mats, 77);
    // every pixel has its texture; the quarry's timber post (upright) has its grain along y
    const MaterialId timber = mats.idOf("timber");
    const int vy = a.heightPx() - 360;
    std::size_t bad = 0, post = 0;
    for (const Chunk* ch : a.chunks())
        for (int i = 0; i < kChunkSize * kChunkSize; ++i) {
            const Cell& c = ch->cells[static_cast<std::size_t>(i)];
            if (c.material == kEmpty) continue;
            const int x = ch->originX() + (i & kChunkMask), y = ch->originY() + (i >> kChunkShift);
            const bool inPost = c.material == timber && x >= 395 && x <= 404 && y >= vy + 280 && y <= vy + 339;
            post += inPost;
            bad += c.shade != mats[c.material].shadeAt(x, y, inPost ? 1 : 0);
        }
    CHECK_EQ(bad, std::size_t{0});
    CHECK(post > 200);
    // the shade never steers the sim: scrambled shades simulate the same
    Rng rng(9);
    for (Chunk* ch : b.chunks())
        for (Cell& c : ch->cells) c.shade = static_cast<std::uint8_t>(rng.below(256));
    ThreadPool pool(2);
    Simulator sa(a, mats, &pool, 5), sb(b, mats, &pool, 5);
    for (int t = 0; t < 120; ++t) { sa.tick(); sb.tick(); }
    std::size_t differ = 0;
    for (std::size_t k = 0; k < a.chunks().size(); ++k)
        for (std::size_t i = 0; i < std::size_t{kChunkSize} * kChunkSize; ++i) {
            const Cell &ca = a.chunks()[k]->cells[i], &cb = b.chunks()[k]->cells[i];
            differ += ca.material != cb.material || ca.temperature != cb.temperature || ca.vx != cb.vx || ca.vy != cb.vy;
        }
    CHECK_EQ(differ, std::size_t{0});
}
