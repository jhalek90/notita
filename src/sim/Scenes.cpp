#include "Scenes.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace notita::scenes {

void fillRect(World& w, MaterialId m, int x0, int y0, int x1, int y1, Rng& rng) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (Cell* c = w.cell(x, y)) {
                *c = Cell{};
                c->material = m;
                c->shade = static_cast<std::uint8_t>(rng.below(256));
            }
    w.wakeRect(x0 - 1, y0 - 1, x1 + 1, y1 + 1);
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
    auto rect = [&](MaterialId m, int x0, int y0, int x1, int y1) { fillRect(w, m, vx + x0, vy + y0, vx + x1, vy + y1, rng); };

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

void churn(World& w, const MaterialRegistry& mats, std::uint64_t seed) {
    Rng rng(seed);
    w.clear();
    const MaterialId pick[] = {
        mats.idOf("sand"), mats.idOf("sand"), mats.idOf("cinder_grit"),
        mats.idOf("water"), mats.idOf("water"), mats.idOf("lamp_oil"),
        mats.idOf("amber_syrup"), mats.idOf("quickmetal"), mats.idOf("steam"),
    };
    for (Chunk* ch : w.chunks()) {
        for (Cell& c : ch->cells) {
            c = Cell{};
            if (rng.below(100) < 70) c.material = pick[rng.below(static_cast<std::uint32_t>(std::size(pick)))];
            c.shade = static_cast<std::uint8_t>(rng.below(256));
        }
    }
    w.wakeAll();
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
