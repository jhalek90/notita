// Phase 6: the App's lighting glue. Fills the per-pixel light inputs from the
// view (emission by material and temperature, opacity by material, gas from
// the grid, the sky above the view), runs the GPU pass (or the CPU reference
// when there is no GPU path), the "Lighting" panel and --light-selftest.
#include "App.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace notita {

namespace {

// "1.6, 0.5, 0.1" -> three floats; false when malformed
bool parseRgb(const std::string& s, float out[3]) {
    const char* p = s.c_str();
    for (int k = 0; k < 3; ++k) {
        char* end = nullptr;
        out[k] = std::strtof(p, &end);
        if (end == p) return false;
        p = end;
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
    }
    return true;
}

float srgbToLinear(float v) { return v * v; } // the composite's gamma-2 space

constexpr int kSkyScanPx = 256; // how far above the view the sky test looks

} // namespace

void App::initLighting() {
    // per material: emitted light (`emissive = r, g, b` in materials.txt, linear, 0..4)
    // and opacity per pixel (`opacity = 0..1`, else by kind: solids and powders
    // block most light, liquids some, gases little)
    matEmit_.assign(mats_.size(), {0, 0, 0, 0});
    for (std::size_t i = 0; i < mats_.size(); ++i) {
        const Material& m = mats_[static_cast<MaterialId>(i)];
        auto& e = matEmit_[i];
        switch (m.kind) {
        case MaterialKind::Solid: case MaterialKind::Powder: e[3] = 0.6f; break;
        case MaterialKind::Liquid: e[3] = 0.15f; break;
        case MaterialKind::Gas: e[3] = 0.03f; break;
        default: e[3] = 0.0f; break;
        }
        if (auto it = m.extra.find("opacity"); it != m.extra.end()) e[3] = std::clamp(std::strtof(it->second.c_str(), nullptr), 0.0f, 1.0f);
        float rgb[3];
        if (auto it = m.extra.find("emissive"); it != m.extra.end()) {
            if (parseRgb(it->second, rgb))
                for (int k = 0; k < 3; ++k) e[static_cast<std::size_t>(k)] = std::clamp(rgb[k], 0.0f, kEmitMax);
            else SDL_Log("materials: '%s' has a bad emissive value '%s'", m.name.c_str(), it->second.c_str());
        } else if (m.isFlame()) {
            // flames glow in their own colour even without the key
            for (int k = 0; k < 3; ++k) e[static_cast<std::size_t>(k)] = 1.2f * srgbToLinear(static_cast<float>((m.color >> (8 * k)) & 0xFF) / 255.0f);
        }
    }
    // incandescence: dull red from ~450 C to white-yellow past 1500 C (the old glow ramp, now as light)
    static const float stops[][4] = {
        {400, 0.45f, 0.02f, 0.00f}, {650, 0.85f, 0.15f, 0.02f}, {950, 1.00f, 0.50f, 0.08f}, {1300, 1.00f, 0.85f, 0.40f},
        {1675, 1.00f, 1.00f, 0.85f},
    };
    for (int i = 0; i < 256; ++i) {
        const float t = 400.0f + 5.0f * static_cast<float>(i);
        float c[3] = {stops[4][1], stops[4][2], stops[4][3]};
        for (int s = 1; s < 5; ++s)
            if (t <= stops[s][0]) {
                const float f = (t - stops[s - 1][0]) / (stops[s][0] - stops[s - 1][0]);
                for (int k = 0; k < 3; ++k) c[k] = stops[s - 1][k + 1] + (stops[s][k + 1] - stops[s - 1][k + 1]) * f;
                break;
            }
        const float x = std::clamp((t - 420.0f) / 900.0f, 0.0f, 1.0f);
        const float intensity = 0.6f * x * std::sqrt(x);
        for (int k = 0; k < 3; ++k) heatEmit_[static_cast<std::size_t>(i * 4 + k)] = srgbToLinear(c[k]) * intensity;
    }

    matPacked_.resize(matEmit_.size());
    for (std::size_t i = 0; i < matEmit_.size(); ++i)
        matPacked_[i] = packEmit(matEmit_[i][0], matEmit_[i][1], matEmit_[i][2], matEmit_[i][3]);

    lightOn_ = !opts_.noLight;
    lightIn_.resize(kViewW, kViewH);
    lightParamsCpu_ = lightParams_;
    lightParamsCpu_.cellPx = 4; // 160x90 cells, 16 cones of 48 cells: the same 192 px range, a few ms on the pool
    lightParamsCpu_.dirs = 16;
    lightParamsCpu_.range = 48;
    lightParamsCpu_.bloomRadius = 3;
    lightParamsCpu_.bloomSigma = 1.5f;

    gpuLight_ = std::make_unique<GpuLight>();
    const char* base = SDL_GetBasePath();
    if (!renderer_.storageWritable()) gpuLightError_ = "the view texture cannot be a compute target";
    else if (!gpuLight_->init(device_, std::string(base ? base : "") + "shaders/")) gpuLightError_ = gpuLight_->error();
    if (!gpuLightError_.empty()) {
        SDL_Log("lighting: GPU pass unavailable (%s), lighting on the CPU at %d px cells", gpuLightError_.c_str(), lightParamsCpu_.cellPx);
        gpuLight_.reset();
    } else {
        SDL_Log("lighting: GPU pass ready%s", opts_.lightCpu ? " (using the CPU one, --light-cpu)" : "");
    }
    useGpuLight_ = gpuLight_ && !opts_.lightCpu;
}

bool App::lightingActive() const { return lightOn_ && !showTemp_ && !showElectric_ && gasView_ == 0; }

void App::prepareLight() {
    if (!lightingActive() || matEmit_.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    const auto& px = renderer_.pixels();
    std::memcpy(lightIn_.albedo.data(), px.data(), px.size() * sizeof(std::uint32_t));
    const int cx0 = static_cast<int>(camX_), cy0 = static_cast<int>(camY_);
    const std::size_t nm = matEmit_.size();
    auto emitOf = [&](const Cell& c) {
        const std::size_t id = c.material < nm ? c.material : 0;
        const bool hot = c.temperature > 420.0f && !mats_[c.material].isFlame();
        if (!hot && !(c.flags & kFlagBurning)) return matPacked_[id]; // the common case: a table lookup
        const auto& m = matEmit_[id];
        float r = m[0], g = m[1], b = m[2];
        if (hot) {
            // scaled by the pixel's shade, so a glowing pool keeps its grain instead of a flat blob
            const auto gi = static_cast<std::size_t>(std::min(static_cast<int>((c.temperature - 400.0f) * 0.2f), 255)) * 4;
            const float s = 0.7f + 0.6f / 255.0f * static_cast<float>(c.shade);
            r += heatEmit_[gi] * s; g += heatEmit_[gi + 1] * s; b += heatEmit_[gi + 2] * s;
        }
        if (c.flags & kFlagBurning) { r += 0.9f; g += 0.32f; b += 0.06f; }
        return packEmit(r, g, b, m[3]);
    };

    // world cells, a view row per job (read-only; the sim is idle while we draw).
    // Outside the world is dark and blocks light.
    pool_->parallelFor(kViewH, [&](int y) {
        std::uint32_t* row = &lightIn_.emit[static_cast<std::size_t>(y) * kViewW];
        std::fill(row, row + kViewW, 0xFF000000u);
        const int wy = cy0 + y;
        for (int ccx = cx0 >> kChunkShift; ccx <= (cx0 + kViewW - 1) >> kChunkShift; ++ccx) {
            const Chunk* ch = world_->chunk(ccx, wy >> kChunkShift);
            if (!ch) continue;
            const int ox = ccx * kChunkSize;
            const int x0 = std::max(ox, cx0), x1 = std::min(ox + kChunkSize, cx0 + kViewW);
            const Cell* cells = &ch->at(0, wy & kChunkMask);
            for (int x = x0; x < x1; ++x) row[x - cx0] = emitOf(cells[x - ox]);
        }
    });
    addFlameLight(); // #480: the flames on the grid
    // flying pixels (sparks, splashes, debris) light and shade like grid pixels
    for (const Particle& p : particles_->particles()) {
        const int x = static_cast<int>(std::floor(p.x)) - cx0, y = static_cast<int>(std::floor(p.y)) - cy0;
        if (x < 0 || y < 0 || x >= kViewW || y >= kViewH) continue;
        lightIn_.emit[static_cast<std::size_t>(y) * kViewW + x] = emitOf(p.cell);
    }
    // gas in the grid dims the light a little (smoke more, by its material's opacity)
    if (gas_ && showGas_ && gas_->params().enabled) {
        const GasFields& f = gas_->fields();
        if (f.w > 0) {
            const std::size_t n = f.cells();
            const int gx0 = gas_->originX(), gy0 = gas_->originY();
            // opacity per gas cell over the view first (16 px each), then add it to the pixels
            const int i0 = std::max(0, (cx0 - gx0) / kGasCell), i1 = std::min(f.w - 1, (cx0 + kViewW - 1 - gx0) / kGasCell);
            const int j0 = std::max(0, (cy0 - gy0) / kGasCell), j1 = std::min(f.h - 1, (cy0 + kViewH - 1 - gy0) / kGasCell);
            const int gw = i1 - i0 + 1;
            if (gw > 0 && j1 >= j0) {
                gasOpacity_.assign(static_cast<std::size_t>(gw) * (j1 - j0 + 1), 0);
                bool any = false;
                for (int j = j0; j <= j1; ++j)
                    for (int i = i0; i <= i1; ++i) {
                        const std::size_t c = static_cast<std::size_t>(j) * f.w + i;
                        float a = 0.0f;
                        for (int k = 0; k < f.species; ++k) {
                            const int mk = f.mass[static_cast<std::size_t>(k) * n + c];
                            if (mk > 0) a += static_cast<float>(mk) / (kGasMassOne * kGasCellPx) * matEmit_[gas_->species(k)][3];
                        }
                        const auto ab = static_cast<std::uint8_t>(std::min(1.0f, a) * 255.0f + 0.5f);
                        gasOpacity_[static_cast<std::size_t>(j - j0) * gw + (i - i0)] = ab;
                        any = any || ab > 0;
                    }
                for (int y = 0; any && y < kViewH; ++y) {
                    const int wy = cy0 + y;
                    if (wy < gy0) continue;
                    const int j = (wy - gy0) / kGasCell;
                    if (j < j0 || j > j1) continue;
                    std::uint32_t* row = &lightIn_.emit[static_cast<std::size_t>(y) * kViewW];
                    const std::uint8_t* go = &gasOpacity_[static_cast<std::size_t>(j - j0) * gw];
                    for (int x = 0; x < kViewW; ++x) {
                        const int wx = cx0 + x;
                        if (wx < gx0) continue;
                        const int i = (wx - gx0) / kGasCell;
                        if (i < i0 || i > i1 || !go[i - i0]) continue;
                        const std::uint32_t e = row[x];
                        const std::uint32_t na = std::min<std::uint32_t>(255u, (e >> 24) + go[i - i0]);
                        row[x] = (e & 0x00FFFFFFu) | (na << 24);
                    }
                }
            }
        }
    }
    // sky reaching the top of the view: straight up through the world, chunk by chunk;
    // past the loaded world (or kSkyScanPx up) it counts as open sky
    pool_->parallelFor(kViewW, [&](int x) {
        const int wx = cx0 + x;
        float T = 1.0f;
        int y = cy0 - 1;
        while (y >= cy0 - kSkyScanPx && T > 0.0f) {
            const Chunk* ch = world_->chunkAtPixel(wx, y);
            if (!ch) break;
            const int oy = ch->originY(), lx = wx - ch->originX();
            for (; y >= oy && y >= cy0 - kSkyScanPx; --y) {
                T *= 1.0f - matEmit_[ch->at(lx, y - oy).material][3];
                if (T < 0.004f) { T = 0.0f; break; }
            }
        }
        lightIn_.topSky[static_cast<std::size_t>(x)] = T;
    });
    lightInputMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // the shared knobs follow the panel; the CPU set keeps its coarser grid
    const LightParams cpu = lightParamsCpu_;
    lightParamsCpu_ = lightParams_;
    lightParamsCpu_.cellPx = cpu.cellPx; lightParamsCpu_.dirs = cpu.dirs; lightParamsCpu_.range = cpu.range;
    lightParamsCpu_.bloomRadius = cpu.bloomRadius; lightParamsCpu_.bloomSigma = cpu.bloomSigma;
    if (!(useGpuLight_ && gpuLight_)) cpuLight_.run(lightIn_, lightParamsCpu_, renderer_.pixels(), pool_.get());
}

bool App::recordLight(SDL_GPUCommandBuffer* cmd) {
    if (!lightingActive() || !useGpuLight_ || !gpuLight_) return false;
    return gpuLight_->record(cmd, lightIn_, lightParams_, renderer_.texture());
}

void App::lightScreenshot() {
    if (!lightingActive() || !useGpuLight_ || !gpuLight_) return; // the CPU path lit the pixels already
    if (!gpuLight_->runSync(lightIn_, lightParams_, renderer_.texture(), renderer_.pixels()))
        SDL_Log("lighting: could not read the lit view back for the screenshot");
}

void App::drawLightUi() {
    const ImGuiIO& io = ImGui::GetIO(); // right edge, between the debug overlay and the player panel
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10, io.DisplaySize.y * 0.35f), ImGuiCond_FirstUseEver, ImVec2(1, 0));
    ImGui::Begin("Lighting", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
    ImGui::Checkbox("Lighting (F4)", &lightOn_);
    if (gpuLight_) {
        ImGui::SameLine();
        ImGui::Checkbox("GPU", &useGpuLight_);
    } else {
        ImGui::TextDisabled("GPU pass unavailable: %s", gpuLightError_.c_str());
    }
    if (lightOn_ && !lightingActive()) ImGui::TextDisabled("(off in the temperature / velocity / pressure views)");
    LightParams& p = lightParams_;
    ImGui::SliderFloat("Ambient", &p.ambient, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Sky", &p.sky, 0.0f, 1.5f, "%.2f");
    ImGui::SliderFloat("Light gain", &p.gain, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat("Haze", &p.haze, 0.0f, 0.3f, "%.2f");
    ImGui::SliderFloat("Bloom", &p.bloom, 0.0f, 2.0f, "%.2f");
    const bool gpu = useGpuLight_ && gpuLight_;
    const LightParams& used = gpu ? lightParams_ : lightParamsCpu_;
    ImGui::Text("%s: %d px cells, %d cones x4, %d cells long", gpu ? "GPU" : "CPU", used.cellPx, used.dirs, used.range);
    ImGui::Text("inputs %.2f ms (CPU)", lightInputMs_);
    if (gpu) {
        if (ImGui::Button("Measure GPU")) lightGpuMs_ = gpuLight_->benchmark(lightParams_, renderer_.texture(), 20);
        ImGui::SameLine();
        if (lightGpuMs_ > 0) ImGui::Text("light pass %.3f ms (GPU)", lightGpuMs_);
        else ImGui::TextDisabled("light pass: not measured");
    } else {
        ImGui::Text("light pass %.2f ms (CPU, %d threads)", cpuLight_.lastMs(), pool_->threadCount());
    }
    ImGui::PopItemWidth();
    ImGui::End();
}

int App::lightSelfTest() {
    // The same inputs through both paths: a synthetic scene (sky, walls, a cave,
    // a lava pool, point lights, translucent liquid) and a real one (forge).
    // Ray cells are integer-stepped on both sides, so only float rounding and
    // the GPU's unorm conversion differ: allow a few levels per channel.
    if (!gpuLight_) {
        SDL_Log("light self-test: no GPU pass (%s)", gpuLightError_.c_str());
        return 2;
    }
    LightInputs syn;
    syn.resize(kViewW, kViewH);
    for (int y = 0; y < kViewH; ++y)
        for (int x = 0; x < kViewW; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * kViewW + x;
            const std::uint32_t h = (static_cast<std::uint32_t>(x) * 73856093u) ^ (static_cast<std::uint32_t>(y) * 19349663u);
            const std::uint32_t g = 60 + (h >> 7) % 120;
            syn.albedo[i] = 0xFF000000u | g | ((g * 7 / 8) << 8) | ((g * 3 / 4) << 16);
            float opacity = 0.0f, er = 0, eg = 0, eb = 0;
            const bool ground = y > 250 + static_cast<int>(20.0 * std::sin(x * 0.03));
            const bool cave = x > 380 && x < 600 && y > 120 && y < 220;
            const bool roof = x > 360 && x < 620 && y > 100 && y < 240 && !cave;
            if (ground || roof) opacity = 0.6f;
            if (ground && x > 100 && x < 200 && y < 290) { opacity = 0.3f; er = 1.6f; eg = 0.5f; eb = 0.1f; } // lava pool
            if (x > 260 && x < 330 && y > 200 && y < 260 && !ground) opacity = 0.15f;                       // water
            if ((x - 450) * (x - 450) + (y - 180) * (y - 180) < 9) { er = 3.0f; eg = 2.0f; eb = 0.8f; }   // lamp in the cave
            if ((x - 40) * (x - 40) + (y - 60) * (y - 60) < 4) { er = 0.4f; eg = 1.5f; eb = 3.5f; }       // blue spark
            syn.emit[i] = packEmit(er, eg, eb, opacity);
        }
    for (int x = 0; x < kViewW; ++x) syn.topSky[static_cast<std::size_t>(x)] = x < 500 ? 1.0f : 0.0f;

    auto compare = [&](const char* what, const LightInputs& in, const LightParams& p) {
        std::vector<std::uint32_t> cpu, gpu;
        cpuLight_.run(in, p, cpu, pool_.get());
        const double cpuMs = cpuLight_.lastMs();
        if (!gpuLight_->runSync(in, p, renderer_.texture(), gpu)) {
            SDL_Log("  %s: GPU run failed: %s", what, SDL_GetError());
            return false;
        }
        int maxd = 0;
        long long sum = 0, over = 0;
        for (std::size_t i = 0; i < cpu.size(); ++i)
            for (int k = 0; k < 3; ++k) {
                const int d = std::abs(static_cast<int>((cpu[i] >> (8 * k)) & 0xFF) - static_cast<int>((gpu[i] >> (8 * k)) & 0xFF));
                maxd = std::max(maxd, d);
                sum += d;
                if (d > 1) ++over;
            }
        const double gpuMs = gpuLight_->benchmark(p, renderer_.texture(), 50);
        const bool ok = maxd <= 3;
        SDL_Log("  %s, %d px cells, %d cones x4: max channel diff %d / 255, mean %.4f, %lld channels > 1 -> %s",
                what, p.cellPx, p.dirs, maxd, static_cast<double>(sum) / (cpu.size() * 3.0), over, ok ? "ok" : "MISMATCH");
        SDL_Log("    GPU light pass %.3f ms (kernels only, mean of 50), CPU reference %.2f ms on %d threads", gpuMs, cpuMs,
                pool_->threadCount());
        return ok;
    };
    SDL_Log("light self-test, view %dx%d:", kViewW, kViewH);
    bool ok = compare("synthetic", syn, lightParams_);
    ok = compare("synthetic (CPU preset)", syn, lightParamsCpu_) && ok;
    // the forge after a few seconds: real emission, gas and sky inputs
    lightOn_ = true;
    showTemp_ = false;
    gasView_ = 0;
    loadScene("forge");
    for (int t = 0; t < 180; ++t) tickSim();
    const bool keep = useGpuLight_;
    useGpuLight_ = true; // only fill the inputs
    drawWorld();
    useGpuLight_ = keep;
    SDL_Log("  forge inputs %.2f ms", lightInputMs_);
    ok = compare("forge", lightIn_, lightParams_) && ok;
    SDL_Log("  %s", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace notita
