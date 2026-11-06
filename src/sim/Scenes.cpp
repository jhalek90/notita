#include "Scenes.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

namespace notita::scenes {

void fillRect(World& w, MaterialId m, int x0, int y0, int x1, int y1, Rng& rng, float temperature) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (Cell* c = w.cell(x, y)) {
                *c = Cell{};
                c->material = m;
                c->temperature = temperature;
                c->shade = static_cast<std::uint8_t>(rng.below(256));
            }
    w.wakeRect(x0 - 1, y0 - 1, x1 + 1, y1 + 1);
}

void fillSpawn(World& w, const MaterialRegistry& mats, MaterialId m, int x0, int y0, int x1, int y1, Rng& rng) {
    fillRect(w, m, x0, y0, x1, y1, rng, mats[m].spawnTemp);
}

void demo(World& w, const MaterialRegistry& mats, std::uint64_t seed) {
    Rng rng(seed);
    w.clear();
    const int X = w.minX(), Y = w.minY(), W = w.widthPx(), H = w.heightPx();
    const MaterialId slate = mats.idOf("slate"), bedrock = mats.idOf("bedrock");
    const MaterialId sand = mats.idOf("sand"), grit = mats.idOf("cinder_grit");
    const MaterialId water = mats.idOf("water"), oil = mats.idOf("lamp_oil");
    const MaterialId syrup = mats.idOf("amber_syrup"), metal = mats.idOf("quickmetal");
    const MaterialId steam = mats.idOf("steam"), murk = mats.idOf("murk");

    // laid out in the 640x360 box at the bottom centre of the world, where the app's camera starts
    const int vx = X + std::max(0, (W - 640) / 2), vy = Y + std::max(0, H - 360);
    auto rect = [&](MaterialId m, int x0, int y0, int x1, int y1) { fillSpawn(w, mats, m, vx + x0, vy + y0, vx + x1, vy + y1, rng); };

    fillRect(w, bedrock, X, Y + H - 8, X + W - 1, Y + H - 1, rng); // floor
    rect(slate, 200, 250, 205, 351);   // basin walls
    rect(slate, 440, 250, 445, 351);
    rect(slate, 30, 150, 170, 155);    // left ledge
    rect(slate, 480, 200, 620, 205);   // right ledge

    // liquids poured into the basin from above: they sort themselves by density
    rect(oil, 250, 70, 320, 115);
    rect(water, 330, 50, 430, 125);
    rect(metal, 300, 20, 320, 35);
    rect(grit, 215, 20, 245, 60);
    // a sand pile and a syrup blob on the ledges
    rect(sand, 60, 50, 130, 149);
    rect(syrup, 510, 150, 570, 199);
    // gases: steam rises out of the bottom corner, murk sinks onto the right ledge
    rect(steam, 470, 300, 540, 340);
    rect(murk, 560, 20, 620, 50);
}

namespace {

template <std::size_t N>
void randomFill(World& w, const MaterialRegistry& mats, const MaterialId (&pick)[N], std::uint64_t seed) {
    Rng rng(seed);
    w.clear();
    for (Chunk* ch : w.chunks()) {
        for (Cell& c : ch->cells) {
            c = Cell{};
            if (rng.below(100) < 70) {
                c.material = pick[rng.below(static_cast<std::uint32_t>(N))];
                c.temperature = mats[c.material].spawnTemp;
            }
            c.shade = static_cast<std::uint8_t>(rng.below(256));
        }
    }
    w.wakeAll();
}

} // namespace

void churn(World& w, const MaterialRegistry& mats, std::uint64_t seed) {
    const MaterialId pick[] = {
        mats.idOf("sand"), mats.idOf("sand"), mats.idOf("cinder_grit"),
        mats.idOf("water"), mats.idOf("water"), mats.idOf("lamp_oil"),
        mats.idOf("amber_syrup"), mats.idOf("quickmetal"), mats.idOf("steam"),
    };
    randomFill(w, mats, pick, seed);
}

void thermalChurn(World& w, const MaterialRegistry& mats, std::uint64_t seed) {
    auto id = [&](const char* n) {
        const MaterialId m = mats.idOf(n);
        return m != kEmpty ? m : mats.idOf("sand"); // test tables may not have every material
    };
    const MaterialId pick[] = {
        id("sand"), id("cinder_grit"), id("water"), id("water"), id("lamp_oil"),
        id("magma"), id("magma"), id("frost"), id("timber"), id("etchant"),
        id("iron"), id("steam"), id("quickmetal"), id("ash"),
    };
    randomFill(w, mats, pick, seed);
}

void forge(World& w, const MaterialRegistry& mats, std::uint64_t seed) {
    Rng rng(seed);
    w.clear();
    const int X = w.minX(), Y = w.minY(), W = w.widthPx(), H = w.heightPx();
    auto id = [&](const char* n) { return mats.idOf(n); };
    const int vx = X + std::max(0, (W - 640) / 2), vy = Y + std::max(0, H - 360);
    auto rect = [&](const char* m, int x0, int y0, int x1, int y1) {
        fillSpawn(w, mats, id(m), vx + x0, vy + y0, vx + x1, vy + y1, rng);
    };
    auto hot = [&](const char* m, float t, int x0, int y0, int x1, int y1) {
        fillRect(w, id(m), vx + x0, vy + y0, vx + x1, vy + y1, rng, t);
    };

    fillRect(w, id("bedrock"), X, Y + H - 8, X + W - 1, Y + H - 1, rng); // floor

    // magma basin; a water shelf above it spills off its open right end onto the magma
    rect("slate", 14, 270, 19, 351);
    rect("slate", 146, 270, 151, 351);
    rect("magma", 20, 300, 145, 351);
    rect("slate", 8, 200, 120, 205);
    rect("slate", 8, 160, 13, 199);
    rect("water", 14, 165, 110, 199);

    // timber hut over a pool of lamp oil, set alight at one corner
    rect("slate", 196, 340, 312, 351);
    rect("lamp_oil", 200, 330, 308, 339);
    rect("timber", 204, 270, 209, 329);
    rect("timber", 299, 270, 304, 329);
    rect("timber", 200, 262, 308, 269);
    hot("timber", 650.0f, 204, 300, 209, 329);

    // etchant poured onto an iron plate (it bubbles hydrogen and drops rust salt)
    rect("slate", 336, 300, 341, 351);
    rect("slate", 424, 300, 429, 351);
    rect("iron", 342, 336, 423, 351);
    rect("etchant", 350, 260, 415, 300);

    // a frost block beside a cloud of steam, and a heap of red-hot sand that melts into glass
    rect("frost", 455, 290, 515, 351);
    rect("steam", 455, 200, 540, 240);
    hot("sand", 1350.0f, 560, 290, 620, 351);
}

void flipVertical(World& w) {
    const int X = w.minX(), Y = w.minY(), W = w.widthPx(), H = w.heightPx();
    for (int y = 0; y < H / 2; ++y)
        for (int x = X; x < X + W; ++x) {
            Cell* a = w.cell(x, Y + y);
            Cell* b = w.cell(x, Y + H - 1 - y);
            if (a && b) std::swap(*a, *b);
        }
    w.wakeAll();
}

} // namespace notita::scenes
