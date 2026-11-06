#include "Brush.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace notita {

int applyBrush(World& world, const MaterialRegistry& mats, const Brush& brush, int x, int y, Rng& rng) {
    const int r = std::max(brush.radius, 0);
    const int r2 = r * r + r; // slightly rounder circles at small radii
    const auto threshold = static_cast<std::uint32_t>(std::clamp(brush.density, 0.0f, 1.0f) * 65536.0f);
    int changed = 0;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            if (brush.shape == BrushShape::Circle && dx * dx + dy * dy > r2) continue;
            Cell* c = world.cell(x + dx, y + dy);
            if (!c) continue;
            if (brush.mode == BrushMode::Heat || brush.mode == BrushMode::Cool) {
                const float d2 = static_cast<float>(dx * dx + dy * dy) / static_cast<float>(std::max(r2, 1));
                const float falloff = 1.0f - 0.75f * std::min(d2, 1.0f);
                world.addHeat(x + dx, y + dy, (brush.mode == BrushMode::Heat ? brush.heat : -brush.heat) * falloff);
                ++changed;
                continue;
            }
            if (brush.mode == BrushMode::Erase) {
                if (c->material == kEmpty) continue;
                world.place(x + dx, y + dy, kEmpty, 0);
                ++changed;
                continue;
            }
            if (threshold < 65536 && !rng.chance16(threshold)) continue;
            if (brush.mode == BrushMode::Place) {
                const MaterialKind k = mats[c->material].kind;
                if (k != MaterialKind::Empty && k != MaterialKind::Gas) continue;
            }
            if (c->material == brush.material && brush.mode != BrushMode::Replace) continue;
            world.setCell(x + dx, y + dy, mats[brush.material].spawn(static_cast<std::uint8_t>(rng.below(256))));
            ++changed;
        }
    return changed;
}

int strokeBrush(World& world, const MaterialRegistry& mats, const Brush& brush,
                int x0, int y0, int x1, int y1, Rng& rng) {
    const int dx = x1 - x0, dy = y1 - y0;
    const int len = std::max(std::abs(dx), std::abs(dy));
    const int step = std::max(1, brush.radius / 2);
    int changed = 0;
    if (len == 0) return applyBrush(world, mats, brush, x0, y0, rng);
    for (int i = 0; i <= len; i += step) {
        const int x = x0 + static_cast<int>(std::lround(static_cast<double>(dx) * i / len));
        const int y = y0 + static_cast<int>(std::lround(static_cast<double>(dy) * i / len));
        changed += applyBrush(world, mats, brush, x, y, rng);
    }
    if (len % step != 0) changed += applyBrush(world, mats, brush, x1, y1, rng);
    return changed;
}

} // namespace notita
