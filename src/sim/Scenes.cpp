#include "Scenes.h"

#include "Bodies.h"

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

void pinRect(World& w, const MaterialRegistry& mats, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (Cell* c = w.cell(x, y); c && mats[c->material].kind == MaterialKind::Solid) c->aux = kAuxPinned;
}

namespace {

// Scene builders pin every solid they place: ledges and walls stay put, and
// only what the player adds or what the sim creates can become a body.
struct PinnedScene {
    World& w;
    const MaterialRegistry& mats;
    ~PinnedScene() { pinRect(w, mats, w.minX(), w.minY(), w.minX() + w.widthPx() - 1, w.minY() + w.heightPx() - 1); }
};

} // namespace

void demo(World& w, const MaterialRegistry& mats, std::uint64_t seed) {
    Rng rng(seed);
    w.clear();
    PinnedScene pin{w, mats};
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
    PinnedScene pin{w, mats};
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
    rect("slate", 196, 322, 199, 339); // tray walls keep the oil under the hut
    rect("slate", 309, 322, 312, 339);
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

void gasLab(World& w, const MaterialRegistry& mats, std::uint64_t seed) {
    Rng rng(seed);
    w.clear();
    PinnedScene pin{w, mats};
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

    // 1. a pot of water on magma under a frost lid: steam rises, condenses on the lid and drips back
    rect("slate", 10, 236, 150, 241);   // lid holder
    rect("frost", 20, 242, 140, 249);
    rect("slate", 20, 300, 25, 351);
    rect("slate", 135, 300, 140, 351);
    rect("iron", 26, 336, 134, 340);    // pot floor
    rect("magma", 26, 341, 134, 351);
    rect("water", 26, 300, 134, 335);

    // 2. a chimney over a fire: smoke and hot air go up the flue
    rect("slate", 180, 200, 185, 330);
    rect("slate", 225, 200, 230, 330);
    rect("slate", 170, 345, 240, 351);
    rect("timber", 190, 330, 220, 344);
    hot("timber", 700.0f, 195, 330, 215, 336);

    // 3. a cave with hydrogen under its roof and sand heaps; a burning post lights it after a while
    rect("slate", 270, 230, 430, 236);  // roof
    rect("slate", 270, 237, 275, 351);
    rect("slate", 425, 237, 430, 300);
    rect("hydrogen", 276, 237, 424, 252);
    rect("sand", 290, 320, 340, 351);
    rect("sand", 360, 330, 410, 351);
    rect("timber", 410, 320, 414, 351); // a burning post; its flames reach the gas after a while
    hot("timber", 700.0f, 410, 340, 414, 351);

    // 4. heavy murk spills off a shelf into a pit with a pond
    rect("slate", 470, 260, 620, 265);
    rect("slate", 470, 230, 475, 259);
    rect("murk", 476, 200, 600, 259);
    rect("slate", 500, 300, 505, 351);
    rect("slate", 615, 300, 620, 351);
    rect("water", 506, 330, 614, 351);
}

void quarry(World& w, const MaterialRegistry& mats, std::uint64_t seed) {
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
    auto pin = [&](int x0, int y0, int x1, int y1) { pinRect(w, mats, vx + x0, vy + y0, vx + x1, vy + y1); };
    fillRect(w, id("bedrock"), X, Y + H - 8, X + W - 1, Y + H - 1, rng); // floor

    // 1. a waterfall: water held high on a slate shelf spills off its end into a basin far below
    rect("slate", 10, 60, 90, 64);
    rect("slate", 10, 20, 14, 59);
    rect("water", 15, 24, 85, 59);
    rect("slate", 20, 300, 25, 351);
    rect("slate", 150, 300, 155, 351);
    rect("water", 26, 330, 149, 351);
    pin(10, 20, 90, 64);
    pin(20, 300, 155, 351);

    // 2. two cups: tincture poured into water blends; lamp oil poured onto water layers
    rect("slate", 190, 250, 193, 351);
    rect("slate", 250, 250, 253, 351);
    rect("water", 194, 310, 249, 351);
    rect("tincture", 200, 150, 240, 200);
    rect("slate", 270, 250, 273, 351);
    rect("slate", 330, 250, 333, 351);
    rect("water", 274, 310, 329, 351);
    rect("lamp_oil", 280, 150, 320, 200);
    pin(190, 250, 333, 351);

    // 3. slate blocks stacked on a timber post that is lit at the bottom: when it burns through they drop and break
    rect("slate", 360, 340, 440, 351);
    pin(360, 340, 440, 351);
    rect("timber", 395, 280, 404, 339);
    hot("timber", 700.0f, 395, 330, 404, 339);
    rect("slate", 380, 262, 420, 279);  // live: a slab on the post
    rect("iron", 388, 240, 412, 261);   // and an iron block on the slab

    // 4. a boulder teetering over the edge of a ledge, above a sand bed
    rect("slate", 470, 200, 540, 206);
    pin(470, 200, 540, 206);
    rect("basalt", 528, 170, 566, 199); // live: more than half of it hangs over the edge
    rect("sand", 480, 320, 630, 351);
}

void playground(World& w, const MaterialRegistry& mats, std::uint64_t seed, float* spawnX, float* spawnY) {
    Rng rng(seed);
    w.clear();
    const int X = w.minX(), Y = w.minY(), W = w.widthPx(), H = w.heightPx();
    auto id = [&](const char* n) { return mats.idOf(n); };
    // laid out for the app's 1024 x 512 world; x positions are scaled to other widths
    const float sx = static_cast<float>(W) / 1024.0f;
    auto px = [&](int x) { return static_cast<int>(static_cast<float>(x) * sx); };
    auto rect = [&](const char* m, int x0, int y0, int x1, int y1) {
        fillSpawn(w, mats, id(m), X + px(x0), Y + y0, X + px(x1), Y + y1, rng);
    };
    auto air = [&](int x0, int y0, int x1, int y1) { fillRect(w, kEmpty, X + px(x0), Y + y0, X + px(x1), Y + y1, rng); };
    const int G = H - 112; // the ground line (y 400 in a 512 px world)

    rect("bedrock", 0, H - 8, 1023, H - 1);
    rect("slate", 0, G + 24, 1023, H - 9);
    rect("sand", 0, G, 1023, G + 23);
    rect("cinder_grit", 0, G + 14, 1023, G + 23);

    // 1. a hill to dig through (or jet, blast or build over): a slate core under sand and grit
    for (int x = 150; x <= 300; ++x) {
        const float t = static_cast<float>(x - 150) / 150.0f;
        const int top = G - static_cast<int>(56.0f * (1.0f - (2.0f * t - 1.0f) * (2.0f * t - 1.0f)));
        rect("sand", x, top, x, G - 1);
        if (top < G - 14) rect("slate", x, top + 10, x, G - 1);
        if (top < G - 30) rect("cinder_grit", x, top + 4, x, top + 9);
    }

    // 2. a lake to swim in, freeze or cross on a raft (a slate basin, water 10 px below the shore)
    air(326, G, 614, G + 74);
    rect("slate", 326, G, 329, G + 74);
    rect("slate", 611, G, 614, G + 74);
    rect("slate", 326, G + 71, 614, G + 74);
    rect("water", 330, G + 10, 610, G + 70);

    // 3. a cliff with an overhang to swing from, and an updraft shaft beside it over an iron plate
    //    heated by magma underneath (the hot air rises up the shaft; the plate itself is hot)
    rect("slate", 660, G - 200, 720, G - 1);
    rect("slate", 630, G - 200, 659, G - 192);
    rect("slate", 790, G - 170, 798, G - 32);
    rect("slate", 721, G, 789, G + 26);
    rect("magma", 727, G + 2, 783, G + 20);
    rect("iron", 721, G - 4, 789, G - 1);
    rect("glass", 680, G - 212, 700, G - 201); // a beacon on top of the cliff
    rect("iron", 686, G - 220, 694, G - 213);

    // 4. a tub of quickmetal: even slate floats on it
    air(870, G, 970, G + 34);
    rect("slate", 870, G, 873, G + 34);
    rect("slate", 967, G, 970, G + 34);
    rect("slate", 870, G + 31, 970, G + 34);
    rect("quickmetal", 874, G + 6, 966, G + 30);

    pinRect(w, mats, X, Y, X + W - 1, Y + H - 1); // the scene stays put...

    // ...but these are live: two timber planks on the lake and a slate slab on the quickmetal become rafts
    rect("timber", 370, G + 2, 417, G + 7);
    rect("timber", 510, G + 2, 557, G + 7);
    rect("slate", 896, G + 0, 943, G + 5);

    if (spawnX) *spawnX = static_cast<float>(X + px(60)) + 0.5f;
    if (spawnY) *spawnY = static_cast<float>(Y + G);
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
