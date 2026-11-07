// #483: the app side of the top-down regions (src/sim/Gravity.h): setting the
// overlay also sets the world's flat regions, the floor of a region is drawn
// as tiles (its empty pixels), a small panel, and the --scene topdown smoke run.
#include "App.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace notita {

namespace {
std::uint32_t tileHash(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x9E3779B1u ^ (static_cast<std::uint32_t>(y) * 0x85EBCA77u + 0x632BE5ABu);
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
} // namespace

void App::applyOverlay(const WorldOverlay* overlay) {
    gen_->setOverlay(overlay);
    if (overlay && topDownOn_) world_->gravity().set(overlay->flatRegions());
    else world_->gravity().clear();
}

// Empty pixels in a flat region are the floor seen from above: 16 px tiles in
// two warm stone tones with dark grout and a little per-tile and per-pixel
// variation (a pure function of the world position, so it doesn't swim).
void App::drawFloorTiles() {
    if (world_->gravity().empty() || showTemp_) return;
    auto& px = renderer_.pixels();
    const int cx0 = static_cast<int>(camX_), cy0 = static_cast<int>(camY_);
    for (const FlatRegion& r : world_->gravity().regions()) {
        const int x0 = std::max(r.x0, cx0), x1 = std::min(r.x1, cx0 + kViewW - 1);
        const int y0 = std::max(r.y0, cy0), y1 = std::min(r.y1, cy0 + kViewH - 1);
        const bool sloped = r.slopeX != 0 || r.slopeY != 0;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const Cell* c = world_->cell(x, y);
                if (!c || c->material != kEmpty) continue;
                const int tx = x >> 4, ty = y >> 4, lx = x & 15, ly = y & 15;
                int v;
                if (lx == 0 || ly == 0) v = 70;                        // grout
                else {
                    v = ((tx + ty) & 1) ? 150 : 128;
                    v += static_cast<int>(tileHash(tx, ty) % 14) - 7;
                    v += static_cast<int>(tileHash(x, y) % 9) - 4;
                    if (lx == 1 || ly == 1) v += 10;                   // a lit bevel on each tile
                    if (lx == 15 || ly == 15) v -= 12;
                }
                if (sloped && ((x + y * 2) & 7) == 0) v -= 18;        // a ribbed gutter floor
                const auto ch = [&](float k) { return static_cast<std::uint32_t>(std::clamp(static_cast<float>(v) * k, 0.0f, 255.0f)); };
                px[static_cast<std::size_t>(y - cy0) * kViewW + (x - cx0)] = 0xFF000000u | ch(0.84f) | (ch(0.78f) << 8) | (ch(0.68f) << 16);
            }
    }
}

void App::drawTopDownUi() {
    if (gen_->overlay() != showcase_.get()) return;
    ImGui::Separator();
    const bool here = playerOn_ && player_->topDown();
    ImGui::Text("Top-down room: %s", here ? "you're in it (seen from above)" : "east of the course");
    if (ImGui::Checkbox("Gravity into the screen there", &topDownOn_)) applyOverlay(gen_->overlay());
    if (ImGui::Button("Go to the top-down room")) {
        if (playerOn_) player_->spawn(showcase_->topDownX(), showcase_->topDownY());
        else spawnPlayer(showcase_->topDownX(), showcase_->topDownY(), true);
    }
    ImGui::TextDisabled("Inside: WASD walks 8 ways (no jumping), walking shoves sand,");
    ImGui::TextDisabled("dig/place as usual, E flips the lamp and the hut igniter; dig the gas tank's plug.");
}

// --scene topdown --autoplay: up the gutter to dig a hole in its gate, back
// down ahead of the water, east through the sand pile, a charge by the crate,
// then north past the hut (whose igniter autoShowcase closes at t = 60 with
// every other switch) and east to dig the murk tank's plug.
void App::autoTopDown(PlayerInput& in, int f) {
    const float x = player_->x(), y = player_->y();
    if (f < 14) in.move = 1.0f;
    else if (f < 120) in.moveY = -1.0f;
    else if (f < 140) { in.tool = Tool::Dig; in.use = true; in.aimX = 1560.0f; in.aimY = -173.0f; } // the gutter's gate
    else if (f < 330) in.moveY = y < -1.0f ? 1.0f : 0.0f;
    else if (f < 460) in.move = x < 1688.0f ? 1.0f : 0.0f;
    else if (f == 460) { in.tool = Tool::Blast; usePressed_ = true; in.aimX = 1707.0f; in.aimY = 6.0f; }
    else if (f < 480) {}
    else if (f < 560) in.moveY = y > -86.0f ? -1.0f : 0.0f;
    else if (f < 660) in.move = x < 1790.0f ? 1.0f : 0.0f;
    else if (f < 700) in.moveY = y > -128.0f ? -1.0f : 0.0f;
    else if (f < 740) { in.tool = Tool::Dig; in.use = true; in.aimX = 1819.0f; in.aimY = -150.0f; } // the murk tank's plug
}

} // namespace notita
