// #481: the renderer's light touch on textured materials. The texture itself
// lives in each pixel's shade (src/sim/Texture.*); here solid and powder
// pixels get a hint of relief from their neighbours (lit top and left edges,
// shaded undersides, a soft second row) and go darker where liquid touches
// them. Pixel-exact: nothing is blurred, only each pixel's own colour scaled.
#include "App.h"

#include "imgui.h"

#include <algorithm>

namespace notita {

namespace {
enum : std::uint8_t { kOpen = 0, kDense = 1, kWet = 2 };

std::uint32_t scaleColor(std::uint32_t c, int f, int blueF) { // f: 256 = unchanged
    const std::uint32_t r = std::min<std::uint32_t>(255u, ((c & 0xFFu) * static_cast<std::uint32_t>(f)) >> 8);
    const std::uint32_t g = std::min<std::uint32_t>(255u, (((c >> 8) & 0xFFu) * static_cast<std::uint32_t>(f)) >> 8);
    const std::uint32_t b = std::min<std::uint32_t>(255u, (((c >> 16) & 0xFFu) * static_cast<std::uint32_t>(blueF)) >> 8);
    return (c & 0xFF000000u) | r | (g << 8) | (b << 16);
}
} // namespace

void App::rebuildSurfaceKinds() {
    matSurface_.assign(mats_.size(), kOpen);
    for (std::size_t i = 0; i < mats_.size(); ++i) {
        const MaterialKind k = mats_[static_cast<MaterialId>(i)].kind;
        if (k == MaterialKind::Solid || k == MaterialKind::Powder) matSurface_[i] = kDense;
        else if (k == MaterialKind::Liquid) matSurface_[i] = kWet;
    }
}

void App::applyRelief() {
    if (!relief_ || showTemp_ || reliefStrength_ <= 0.0f) return;
    if (matSurface_.size() != mats_.size()) rebuildSurfaceKinds();
    constexpr int W = kViewW, H = kViewH;
    viewMask_.assign(static_cast<std::size_t>(W) * H, kOpen);
    const int cx0 = static_cast<int>(camX_), cy0 = static_cast<int>(camY_);
    for (int ccy = cy0 >> kChunkShift; ccy <= (cy0 + H - 1) >> kChunkShift; ++ccy)
        for (int ccx = cx0 >> kChunkShift; ccx <= (cx0 + W - 1) >> kChunkShift; ++ccx) {
            const Chunk* ch = world_->chunk(ccx, ccy);
            if (!ch) continue;
            const int ox = ccx * kChunkSize, oy = ccy * kChunkSize;
            const int x0 = std::max(ox, cx0), x1 = std::min(ox + kChunkSize, cx0 + W);
            const int y0 = std::max(oy, cy0), y1 = std::min(oy + kChunkSize, cy0 + H);
            for (int y = y0; y < y1; ++y) {
                std::uint8_t* row = &viewMask_[static_cast<std::size_t>(y - cy0) * W];
                const Cell* cells = &ch->at(0, y - oy);
                for (int x = x0; x < x1; ++x) row[x - cx0] = matSurface_[cells[x - ox].material];
            }
        }
    // edge weights (1/256), scaled by the strength slider
    const float s = reliefStrength_;
    const int lit = static_cast<int>(34.0f * s), litSide = static_cast<int>(14.0f * s), lit2 = static_cast<int>(12.0f * s);
    const int shadow = static_cast<int>(50.0f * s), shadowSide = static_cast<int>(18.0f * s), shadow2 = static_cast<int>(22.0f * s);
    const int wet = static_cast<int>(62.0f * wetness_);
    auto& px = renderer_.pixels();
    auto at = [&](int x, int y) -> std::uint8_t {
        if (x < 0 || y < 0 || x >= W || y >= H) return kDense; // the view border is not an edge
        return viewMask_[static_cast<std::size_t>(y) * W + x];
    };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (viewMask_[static_cast<std::size_t>(y) * W + x] != kDense) continue;
            const std::uint8_t u = at(x, y - 1), d = at(x, y + 1), l = at(x - 1, y), r = at(x + 1, y);
            int f = 256;
            if (u != kDense) f += lit;
            else if (at(x, y - 2) != kDense) f += lit2;
            if (d != kDense) f -= shadow;
            else if (at(x, y + 2) != kDense) f -= shadow2;
            if (l != kDense) f += litSide;
            if (r != kDense) f -= shadowSide;
            int fb = f;
            if (wet > 0 && (u == kWet || d == kWet || l == kWet || r == kWet || at(x, y - 2) == kWet)) {
                // damp: darker and a touch cooler
                f = f * (256 - wet) >> 8;
                fb = fb * (256 - wet * 3 / 4) >> 8;
            }
            if (f == 256 && fb == 256) continue;
            std::uint32_t& c = px[static_cast<std::size_t>(y) * W + x];
            c = scaleColor(c, f, fb);
        }
}

void App::registerTextureUi() { // #484: View > Textures; F12 is in View > Old / new
    menu_.add("View", "Textures", 60, [this] {
        ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
        ImGui::SliderFloat("Relief", &reliefStrength_, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Wet darkening", &wetness_, 0.0f, 2.0f, "%.2f");
        ImGui::PopItemWidth();
        ImGui::TextDisabled("Pixel textures come from palette/texture keys in data/materials.txt");
    });
}

} // namespace notita
