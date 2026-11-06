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

    // floor and a basin
    fillRect(w, bedrock, X, Y + H - 8, X + W - 1, Y + H - 1, rng);
    const int bx0 = X + W / 2 - 120, bx1 = X + W / 2 + 120, by = Y + H - 9;
    fillRect(w, slate, bx0, by - 80, bx0 + 5, by, rng);
    fillRect(w, slate, bx1 - 5, by - 80, bx1, by, rng);
    // ledges
    fillRect(w, slate, X + 40, Y + H / 3, X + 200, Y + H / 3 + 5, rng);
    fillRect(w, slate, X + W - 220, Y + H / 2, X + W - 60, Y + H / 2 + 5, rng);

    // liquids poured into the basin from above: they sort themselves
    fillRect(w, oil, bx0 + 10, Y + 40, bx0 + 70, Y + 70, rng);
    fillRect(w, water, bx0 + 80, Y + 40, bx0 + 150, Y + 80, rng);
    fillRect(w, metal, bx0 + 160, Y + 50, bx0 + 180, Y + 60, rng);
    fillRect(w, syrup, X + 60, Y + H / 3 - 30, X + 110, Y + H / 3 - 1, rng);
    // powders
    fillRect(w, sand, X + 130, Y + 20, X + 180, Y + H / 3 - 1, rng);
    fillRect(w, grit, X + W - 160, Y + 30, X + W - 120, Y + H / 2 - 1, rng);
    // gases
    fillRect(w, steam, X + W / 2 - 20, Y + H - 40, X + W / 2 + 20, Y + H - 20, rng);
    fillRect(w, murk, X + W - 200, Y + 10, X + W - 170, Y + 30, rng);
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
