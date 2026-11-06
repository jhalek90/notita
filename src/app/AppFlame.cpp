// #480: flames as hot glowing gas. The App side of the flame look: the grid
// fields over the view packed per cell (temperature, burn rate, soot, tint),
// the GPU flame pass (src/app/shaders/flame.hlsl), the CPU fallback at sim
// resolution, the flames' light for the lighting pass, the old/new toggle
// (F11) and the "Flames" panel.
#include "App.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace notita {

namespace {

// the same ramp as blackbody() in flame.hlsl (display space)
void flameRamp(float t, float out[3]) {
    static const float stops[7][4] = {
        {500.0f, 0.30f, 0.02f, 0.00f}, {750.0f, 0.80f, 0.10f, 0.01f}, {1000.0f, 1.00f, 0.30f, 0.03f},
        {1250.0f, 1.00f, 0.52f, 0.10f}, {1500.0f, 1.00f, 0.75f, 0.30f}, {1900.0f, 1.00f, 0.93f, 0.75f},
        {2600.0f, 0.80f, 0.90f, 1.25f},
    };
    if (t <= stops[0][0]) { for (int k = 0; k < 3; ++k) out[k] = stops[0][k + 1]; return; }
    for (int s = 1; s < 7; ++s)
        if (t <= stops[s][0]) {
            const float f = (t - stops[s - 1][0]) / (stops[s][0] - stops[s - 1][0]);
            for (int k = 0; k < 3; ++k) out[k] = stops[s - 1][k + 1] + (stops[s][k + 1] - stops[s - 1][k + 1]) * f;
            return;
        }
    for (int k = 0; k < 3; ++k) out[k] = stops[6][k + 1];
}

bool parseRgbList(const std::string& s, float* out, int n) {
    const char* p = s.c_str();
    for (int k = 0; k < n; ++k) {
        char* end = nullptr;
        out[k] = std::strtof(p, &end);
        if (end == p) return k >= 3; // the 4th value (strength) is optional
        p = end;
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
    }
    return true;
}

constexpr int kFlameMargin = 2; // cells around the view (the cubic spline reaches 2 cells)
constexpr float kHotGasGlowCap = 1300.0f; // C: hot gas that isn't burning glows at most this hot

} // namespace

void App::initFlame() {
    // per-fuel tints: `flame_tint = r, g, b[, strength]` (display 0..1) on a burning material
    flameTintIdx_.assign(mats_.size(), 0);
    for (auto& t : flameTints_) t = {0, 0, 0, 0};
    int next = 1;
    for (const Material& m : mats_.all()) {
        auto it = m.extra.find("flame_tint");
        if (it == m.extra.end() || next >= kFlameTints) continue;
        float v[4] = {1, 1, 1, 0.8f};
        if (!parseRgbList(it->second, v, 4)) {
            SDL_Log("materials: '%s' has a bad flame_tint '%s'", m.name.c_str(), it->second.c_str());
            continue;
        }
        flameTints_[static_cast<std::size_t>(next)] = {v[0], v[1], v[2], std::clamp(v[3], 0.0f, 1.0f)};
        flameTintIdx_[m.id] = static_cast<std::uint8_t>(next++);
    }
    newFlames_ = !opts_.oldFlames;
    setNewFlames(newFlames_);

    gpuFlame_ = std::make_unique<GpuFlame>();
    const char* base = SDL_GetBasePath();
    if (!renderer_.storageReadable()) {
        gpuFlameError_ = "the view texture can't be read by compute shaders";
        gpuFlame_.reset();
    } else if (!gpuFlame_->init(device_, std::string(base ? base : "") + "shaders/")) {
        gpuFlameError_ = gpuFlame_->error();
        gpuFlame_.reset();
    }
    if (!gpuFlame_) SDL_Log("flame pass unavailable (%s): flames are drawn at sim resolution", gpuFlameError_.c_str());
}

void App::setNewFlames(bool on) {
    newFlames_ = on;
    if (sim_) sim_->params().gridFlames = on;
    if (gas_) gas_->params().gridFlames = on;
}

bool App::flameActive() const {
    return newFlames_ && gas_ && gas_->params().enabled && showGas_ && !showTemp_ && !showElectric_ && gasView_ == 0;
}

// Packs the grid over the view (plus a margin) and the per-cell flame colour
// used by the light and the CPU fallback.
void App::buildFlameCells() {
    flameAny_ = false;
    if (!flameActive()) return;
    const GasFields& f = gas_->fields();
    if (f.w <= 0) return;
    const int cx0 = static_cast<int>(camX_), cy0 = static_cast<int>(camY_);
    const int gx0 = gas_->originX(), gy0 = gas_->originY();
    auto floorDiv = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    const int i0 = floorDiv(cx0 - gx0, kGasCell) - kFlameMargin, j0 = floorDiv(cy0 - gy0, kGasCell) - kFlameMargin;
    flameCW_ = kViewW / kGasCell + 2 * kFlameMargin + 2;
    flameCH_ = kViewH / kGasCell + 2 * kFlameMargin + 2;
    flameI0_ = i0;
    flameJ0_ = j0;
    flameOffX_ = static_cast<float>(cx0 - gx0) / kGasCell - 0.5f - static_cast<float>(i0);
    flameOffY_ = static_cast<float>(cy0 - gy0) / kGasCell - 0.5f - static_cast<float>(j0);
    const std::size_t nc = static_cast<std::size_t>(flameCW_) * flameCH_;
    flameCells_.assign(nc, FlameCell{});
    flameEmit_.assign(nc * 3, 0.0f);
    const auto& glow = gas_->flameGlow();
    const auto& soot = gas_->soot();
    const float minT = flameLook_.minTemp;
    for (int j = 0; j < flameCH_; ++j) {
        const int gj = j0 + j;
        if (gj < 0 || gj >= f.h) continue;
        for (int i = 0; i < flameCW_; ++i) {
            const int gi = i0 + i;
            if (gi < 0 || gi >= f.w) continue;
            const int c = gj * f.w + gi;
            if (f.open[static_cast<std::size_t>(c)] <= 0.0f) continue;
            FlameCell& fc = flameCells_[static_cast<std::size_t>(j) * flameCW_ + i];
            // the burning gas at its flame heat; hot gas that isn't burning glows at most orange-yellow
            fc.temperature = std::max(std::min(gas_->temperature(c), kHotGasGlowCap), gas_->flameHeat(c));
            fc.burn = glow[static_cast<std::size_t>(c)];
            fc.soot = soot[static_cast<std::size_t>(c)];
            const MaterialId tm = gas_->flameTintMat(c);
            const float tintAmt = gas_->flameTint(c);
            const std::uint8_t ti = tm < flameTintIdx_.size() ? flameTintIdx_[tm] : 0;
            if (ti > 0) fc.tint = static_cast<float>(ti) + std::clamp(tintAmt, 0.0f, 0.99f);
            // the glow without the noise (light emission, CPU fallback)
            const float heat = std::clamp((fc.temperature - minT) / 900.0f, 0.0f, 1.0f);
            const float react = std::clamp(fc.burn * 0.06f, 0.0f, 1.0f);
            const float a = ti > 0 ? std::clamp(tintAmt * 2.5f, 0.0f, 1.0f) * flameTints_[ti][3] : 0.0f;
            const float I = std::min(heat * std::sqrt(heat) * (0.15f + 0.85f * react), 1.1f) * (1.0f - 0.6f * a) * flameLook_.gain;
            if (I <= 0.002f && fc.soot <= 0.002f) continue;
            flameAny_ = true;
            float col[3];
            flameRamp(fc.temperature + 320.0f * react, col);
            if (ti > 0) {
                const auto& t = flameTints_[ti];
                for (int k = 0; k < 3; ++k) col[k] += (t[static_cast<std::size_t>(k)] * (0.6f + 0.6f * heat) - col[k]) * a;
            }
            float* e = &flameEmit_[(static_cast<std::size_t>(j) * flameCW_ + i) * 3];
            for (int k = 0; k < 3; ++k) e[k] = col[k] * I;
        }
    }
}

// bilinear flame colour * intensity at a view pixel (display space)
void App::flameAt(int x, int y, float out[3]) const {
    const float gx = (static_cast<float>(x) + 0.5f) / kGasCell + flameOffX_;
    const float gy = (static_cast<float>(y) + 0.5f) / kGasCell + flameOffY_;
    const int i = static_cast<int>(std::floor(gx)), j = static_cast<int>(std::floor(gy));
    const float fx = gx - static_cast<float>(i), fy = gy - static_cast<float>(j);
    out[0] = out[1] = out[2] = 0.0f;
    for (int q = 0; q < 4; ++q) {
        const int ci = i + (q & 1), cj = j + (q >> 1);
        if (ci < 0 || cj < 0 || ci >= flameCW_ || cj >= flameCH_) continue;
        const float w = ((q & 1) ? fx : 1.0f - fx) * ((q >> 1) ? fy : 1.0f - fy);
        const float* e = &flameEmit_[(static_cast<std::size_t>(cj) * flameCW_ + ci) * 3];
        out[0] += e[0] * w;
        out[1] += e[1] * w;
        out[2] += e[2] * w;
    }
}

// No GPU pass: flames added to the view pixels at sim resolution (bilinear).
void App::drawFlamesCpu() {
    if (!flameAny_ || gpuFlame_ || lightingActive()) return; // lit: the light pass draws them as emission
    auto& px = renderer_.pixels();
    pool_->parallelFor(kViewH, [&](int y) {
        std::uint32_t* row = &px[static_cast<std::size_t>(y) * kViewW];
        for (int x = 0; x < kViewW; ++x) {
            float e[3];
            flameAt(x, y, e);
            if (e[0] + e[1] + e[2] < 0.004f) continue;
            const std::uint32_t c = row[x];
            std::uint32_t o = 0xFF000000u;
            for (int k = 0; k < 3; ++k) {
                const float v = static_cast<float>((c >> (8 * k)) & 0xFF) / 255.0f + e[k];
                o |= static_cast<std::uint32_t>(std::min(1.0f, v) * 255.0f + 0.5f) << (8 * k);
            }
            row[x] = o;
        }
    });
}

// The flames' light: added to the emission of open pixels (the composite also
// shows it as a soft glow; the GPU pass draws the sharp flame on top).
void App::addFlameLight() {
    if (!flameAny_) return;
    const float gain = gpuFlame_ ? flameLightGain_ : 1.0f;
    if (gain <= 0.0f) return;
    const int cx0 = static_cast<int>(camX_), cy0 = static_cast<int>(camY_);
    pool_->parallelFor(kViewH, [&](int y) {
        std::uint32_t* row = &lightIn_.emit[static_cast<std::size_t>(y) * kViewW];
        const int wy = cy0 + y;
        for (int x = 0; x < kViewW; ++x) {
            float e[3];
            flameAt(x, y, e);
            if (e[0] + e[1] + e[2] < 0.004f) continue;
            const Chunk* ch = world_->chunkAtPixel(cx0 + x, wy);
            if (!ch) continue;
            const Cell& cell = ch->at(cx0 + x - ch->originX(), wy - ch->originY());
            const MaterialKind k = mats_[cell.material].kind;
            if (k != MaterialKind::Empty && k != MaterialKind::Gas && k != MaterialKind::Special) continue;
            const std::uint32_t p = row[x];
            float rgb[3];
            for (int q = 0; q < 3; ++q)
                rgb[q] = static_cast<float>((p >> (8 * q)) & 0xFF) / 255.0f * kEmitMax + e[q] * e[q] * gain;
            row[x] = packEmit(rgb[0], rgb[1], rgb[2], static_cast<float>(p >> 24) / 255.0f);
        }
    });
}

bool App::recordFlame(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swap, bool lit, bool liquid) {
    if (!flameActive() || !gpuFlame_ || !flameAny_) return false;
    SDL_GPUTexture* scene = liquid ? gpuLiquid_->output() : renderer_.texture();
    const int sw = liquid ? gpuLiquid_->outW() : kViewW, sh = liquid ? gpuLiquid_->outH() : kViewH;
    if (!lit && !liquid) renderer_.upload(cmd);
    const float time = static_cast<float>(SDL_GetTicks()) / 1000.0f;
    const bool ok = gpuFlame_->record(cmd, scene, sw, sh, flameCells_, flameCW_, flameCH_, flameOffX_, flameOffY_,
                                      static_cast<float>(flameI0_ + gas_->originX() / kGasCell),
                                      static_cast<float>(flameJ0_ + gas_->originY() / kGasCell), kViewW, kViewH, liquidScale(),
                                      time, flameLook_, flameTints_);
    if (!ok) {
        renderer_.blit(cmd, scene, sw, sh, swap, viewport_);
        return true;
    }
    renderer_.blit(cmd, gpuFlame_->output(), gpuFlame_->outW(), gpuFlame_->outH(), swap, viewport_);
    flameDrawn_ = true;
    return true;
}

bool App::flameScreenshot(const std::string& path) {
    if (!flameDrawn_ || !gpuFlame_ || !flameActive()) return false;
    std::vector<std::uint32_t> px;
    if (!gpuFlame_->readBack(px)) return false;
    return writeBmp(path, px, gpuFlame_->outW(), gpuFlame_->outH());
}

void App::drawFlameUi() {
    ImGui::Begin("Flames", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    bool on = newFlames_;
    if (ImGui::Checkbox("Flames on the gas grid (F11 flips old/new)", &on)) setNewFlames(on);
    if (!gpuFlame_) ImGui::TextDisabled("GPU flame pass unavailable: %s", gpuFlameError_.c_str());
    if (gas_) {
        const GasGridStats& st = gas_->stats();
        ImGui::Text("burning cells %d, fuel %.0f, burnt %.0f /tick, max %.0f C", st.flameCells, st.fuel, st.fuelBurned,
                    static_cast<double>(st.maxTemp));
        GasGridParams& gp = gas_->params();
        ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
        ImGui::SliderFloat("Fuel per burning px", &sim_->params().flameFuel, 0.0f, 4.0f, "%.2f");
        ImGui::SliderFloat("Burn rate", &gp.fuelBurnRate, 0.02f, 1.0f, "%.2f");
        ImGui::SliderFloat("Fuel ignites at", &gp.fuelIgnite, 100.0f, 900.0f, "%.0f C");
        ImGui::SliderFloat("Soot yield", &gp.sootYield, 0.0f, 0.05f, "%.3f");
        ImGui::SliderFloat("Brightness", &flameLook_.gain, 0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("Glows from", &flameLook_.minTemp, 200.0f, 900.0f, "%.0f C");
        ImGui::SliderFloat("Soot darkening", &flameLook_.smoke, 0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("Flicker", &flameLook_.flicker, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Light", &flameLightGain_, 0.0f, 2.0f, "%.2f");
        ImGui::PopItemWidth();
    }
    ImGui::TextDisabled("Off = flame pixels (the phase 2 fire)");
    ImGui::End();
}

} // namespace notita
