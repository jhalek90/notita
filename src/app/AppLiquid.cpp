// #477: liquids that look like liquids. The CPU side colours liquid pixels
// smoothly by depth (per-material look in data/materials.txt) and fills the
// per-pixel info the GPU surface pass (GpuLiquid, shaders/liquid.hlsl)
// reads; F10 flips between this and the old look/motion for comparison.
#include "App.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>

namespace notita {

namespace {

// "#rrggbb" or "#rrggbbaa" -> r, g, b, a in 0..1; false when malformed
bool parseHex(const std::string& s, float out[4]) {
    std::size_t i = s.find('#');
    if (i == std::string::npos) return false;
    std::string h = s.substr(i + 1);
    while (!h.empty() && (h.back() == ' ' || h.back() == '\t' || h.back() == '\r')) h.pop_back();
    if (h.size() != 6 && h.size() != 8) return false;
    const unsigned long v = std::strtoul(h.c_str(), nullptr, 16);
    if (h.size() == 6) {
        out[0] = static_cast<float>((v >> 16) & 0xFF) / 255.0f;
        out[1] = static_cast<float>((v >> 8) & 0xFF) / 255.0f;
        out[2] = static_cast<float>(v & 0xFF) / 255.0f;
    } else {
        out[0] = static_cast<float>((v >> 24) & 0xFF) / 255.0f;
        out[1] = static_cast<float>((v >> 16) & 0xFF) / 255.0f;
        out[2] = static_cast<float>((v >> 8) & 0xFF) / 255.0f;
        out[3] = static_cast<float>(v & 0xFF) / 255.0f;
    }
    return true;
}

float extraFloat(const Material& m, const char* key, float def, float lo, float hi) {
    auto it = m.extra.find(key);
    if (it == m.extra.end()) return def;
    return std::clamp(std::strtof(it->second.c_str(), nullptr), lo, hi);
}

void unpack(std::uint32_t c, float out[3]) {
    out[0] = static_cast<float>(c & 0xFF) / 255.0f;
    out[1] = static_cast<float>((c >> 8) & 0xFF) / 255.0f;
    out[2] = static_cast<float>((c >> 16) & 0xFF) / 255.0f;
}

std::uint32_t mixColor(std::uint32_t a, std::uint32_t b, std::uint32_t wb) { // wb: 0..255 weight of b
    auto ch = [&](int s) {
        const std::uint32_t x = (a >> s) & 0xFF, y = (b >> s) & 0xFF;
        return ((x * (255 - wb) + y * wb) / 255) << s;
    };
    return ch(0) | ch(8) | ch(16) | 0xFF000000u;
}

std::uint32_t pack(const float c[3]) {
    auto b = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return b(c[0]) | (b(c[1]) << 8) | (b(c[2]) << 16) | 0xFF000000u;
}

} // namespace

void App::initLiquid() {
    // per liquid: a look slot (1..15) and its depth ramp (64 steps)
    liquidLook_.assign(mats_.size(), 0);
    liquidRamp_.assign(mats_.size() * kLiquidRamp, 0);
    looks_ = {};
    float air[3];
    unpack(mats_[kEmpty].color, air);
    int next = 1;
    for (std::size_t i = 0; i < mats_.size(); ++i) {
        const Material& m = mats_[static_cast<MaterialId>(i)];
        if (m.kind != MaterialKind::Liquid) continue;
        const int slot = std::min(next++, kLiquidLooks - 1);
        liquidLook_[i] = static_cast<std::uint8_t>(slot);
        float base[4] = {0, 0, 0, 1}, deep[4] = {0, 0, 0, 1};
        unpack(m.color, base);
        // default deep colour: the base, darker and a touch more saturated
        for (int k = 0; k < 3; ++k) deep[k] = base[k] * base[k] * 0.8f;
        if (auto it = m.extra.find("look_deep"); it != m.extra.end()) parseHex(it->second, deep);
        const float depthPx = extraFloat(m, "look_depth", 24.0f, 1.0f, 200.0f);
        const float clear = extraFloat(m, "look_clear", 0.3f, 0.0f, 1.0f);
        for (int d = 0; d < kLiquidRamp; ++d) {
            const float t = std::min(static_cast<float>(d) / depthPx, 1.0f);
            const float a = 1.0f - clear * (1.0f - t); // shallow: more of the backdrop shows through
            float c[3];
            for (int k = 0; k < 3; ++k) {
                const float v = base[k] + (deep[k] - base[k]) * t;
                c[k] = air[k] + (v - air[k]) * a;
            }
            liquidRamp_[i * kLiquidRamp + static_cast<std::size_t>(d)] = pack(c);
        }
        LiquidLook& lk = looks_[static_cast<std::size_t>(slot)];
        float shine[4] = {std::min(1.0f, base[0] * 0.5f + 0.55f), std::min(1.0f, base[1] * 0.5f + 0.55f),
                          std::min(1.0f, base[2] * 0.5f + 0.55f), 0.55f};
        if (auto it = m.extra.find("look_shine"); it != m.extra.end()) parseHex(it->second, shine);
        float foam[4] = {std::min(1.0f, base[0] * 0.3f + 0.75f), std::min(1.0f, base[1] * 0.3f + 0.75f),
                         std::min(1.0f, base[2] * 0.3f + 0.75f), 0.8f};
        if (auto it = m.extra.find("look_foam"); it != m.extra.end()) parseHex(it->second, foam);
        std::copy(shine, shine + 4, lk.shine);
        std::copy(foam, foam + 4, lk.foam);
        lk.wobble = extraFloat(m, "look_wobble", 0.35f, 0.0f, 1.5f);
        lk.shimmer = extraFloat(m, "look_shimmer", 0.25f, 0.0f, 1.0f);
        lk.glint = extraFloat(m, "look_glint", 0.6f, 0.0f, 2.0f);
    }
    liquidInfo_.assign(static_cast<std::size_t>(kViewW) * kViewH, 0);

    gpuLiquid_ = std::make_unique<GpuLiquid>();
    const char* base = SDL_GetBasePath();
    if (!renderer_.storageReadable()) {
        gpuLiquidError_ = "the view texture can't be read by compute shaders";
        gpuLiquid_.reset();
    } else if (!gpuLiquid_->init(device_, std::string(base ? base : "") + "shaders/")) {
        gpuLiquidError_ = gpuLiquid_->error();
        gpuLiquid_.reset();
    }
    if (!gpuLiquid_) SDL_Log("liquid surface pass unavailable (%s): liquids keep the smooth colours only", gpuLiquidError_.c_str());
    newLook_ = !opts_.oldLook;
    newMotion_ = !opts_.oldMotion;
    smoothPowder_ = calmLiquid_ = newMotion_;
}

bool App::liquidActive() const { return newLook_ && !showTemp_ && !showElectric_ && gasView_ == 0; }

void App::shadeLiquids() {
    if (!liquidActive()) return;
    auto& px = renderer_.pixels();
    const int cx0 = static_cast<int>(camX_), cy0 = static_cast<int>(camY_);
    const bool glowCpu = glow_ && !lightingActive();
    constexpr int kAbove = 48;    // rows above the view that count towards the depth
    constexpr int kStrip = 64;    // columns per job: each walks its rows top-down
    const int strips = (kViewW + kStrip - 1) / kStrip;
    pool_->parallelFor(strips, [&](int s) {
        const int x0 = s * kStrip, x1 = std::min(kViewW, x0 + kStrip);
        int depth[kStrip] = {};
        bool openPrev[kStrip] = {};
        for (int vy = -kAbove; vy < kViewH; ++vy) {
            const int wy = cy0 + vy;
            for (int vx = x0; vx < x1;) {
                const int wx = cx0 + vx;
                const Chunk* ch = world_->chunkAtPixel(wx, wy);
                const int run = std::min(x1 - vx, kChunkSize - (wx & kChunkMask));
                for (int k = 0; k < run; ++k, ++vx) {
                    const int col = vx - x0;
                    const Cell* c = ch ? &ch->at((wx + k) & kChunkMask, wy & kChunkMask) : nullptr;
                    const MaterialId mid = c ? c->material : kEmpty;
                    const Material& m = mats_[mid];
                    const bool liquid = c && m.kind == MaterialKind::Liquid && !(c->flags & kFlagBody);
                    const bool open = c && (m.kind == MaterialKind::Empty || m.kind == MaterialKind::Gas || m.kind == MaterialKind::Special);
                    if (vy >= 0) {
                        std::uint32_t info = 0;
                        const std::size_t pi = static_cast<std::size_t>(vy) * kViewW + static_cast<std::size_t>(vx);
                        if (liquid) {
                            const int d = depth[col];
                            std::uint32_t colr = liquidRamp_[static_cast<std::size_t>(mid) * kLiquidRamp + static_cast<std::size_t>(std::min(d, kLiquidRamp - 1))];
                            if (glowCpu && c->temperature > 420.0f) {
                                const int gi = std::min(static_cast<int>((c->temperature - 400.0f) * 0.2f), 255);
                                const std::uint32_t g = glowColor_[static_cast<std::size_t>(gi)];
                                colr = mixColor(colr, g | 0xFF000000u, g >> 24);
                            }
                            px[pi] = colr;
                            // a fast falling stream is white water
                            const int fall = std::abs(static_cast<int>(c->vy));
                            // a fast falling pixel at the edge of its stream (open beside it) is white water
                            std::uint32_t foam = 0;
                            if (fall >= 2 * kVelOne) {
                                const Cell* l = world_->cell(wx + k - 1, wy);
                                const Cell* r = world_->cell(wx + k + 1, wy);
                                const bool edge = (l && mats_[l->material].kind != MaterialKind::Liquid) ||
                                                  (r && mats_[r->material].kind != MaterialKind::Liquid);
                                foam = edge ? (fall >= 4 * kVelOne ? 150u : 80u) : 20u;
                            }
                            info = liquidLook_[mid] | (static_cast<std::uint32_t>(std::min(d, 255)) << 8) | (foam << 16) |
                                   (openPrev[col] ? (1u << 25) : 0u);
                            // a fast falling pixel leaves a short streak in the open cells above it
                            // (the pixels of a stream drift apart as they speed up; drawn as a
                            // continuous column of white water instead of scattered dots)
                            const int streak = std::min(fall / kVelOne, 8);
                            for (int s2 = 1; s2 <= streak && vy - s2 >= 0; ++s2) {
                                const std::size_t pa = pi - static_cast<std::size_t>(s2) * kViewW;
                                if (liquidInfo_[pa] != (1u << 24)) break;
                                liquidInfo_[pa] = liquidLook_[mid] | ((foam / 2u) << 16) | (1u << 24);
                                px[pa] = colr;
                            }
                        } else if (open) {
                            info = 1u << 24;
                        }
                        liquidInfo_[pi] = info;
                    }
                    depth[col] = liquid ? depth[col] + 1 : 0;
                    openPrev[col] = open;
                }
            }
        }
    });
}

void App::markLiquidParticle(int vx, int vy, MaterialId m) {
    if (!liquidActive() || m >= liquidLook_.size() || liquidLook_[m] == 0) return;
    const std::size_t pi = static_cast<std::size_t>(vy) * kViewW + static_cast<std::size_t>(vx);
    liquidInfo_[pi] = liquidLook_[m] | (255u << 16);
    renderer_.pixels()[pi] = liquidRamp_[static_cast<std::size_t>(m) * kLiquidRamp];
}

void App::clearLiquidInfo(int vx, int vy) {
    if (!liquidActive() || vx < 0 || vy < 0 || vx >= kViewW || vy >= kViewH) return;
    liquidInfo_[static_cast<std::size_t>(vy) * kViewW + static_cast<std::size_t>(vx)] = 0;
}

int App::liquidScale() const { return std::clamp(static_cast<int>(std::lround(viewport_.scale)), 1, 4); }

bool App::recordLiquid(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swap, bool lit, bool present) {
    if (!liquidActive() || !gpuLiquid_) return false;
    if (!lit) renderer_.upload(cmd);
    liquidTime_ = static_cast<float>(SDL_GetTicks()) / 1000.0f;
    if (!gpuLiquid_->record(cmd, renderer_.texture(), liquidInfo_, kViewW, kViewH, liquidScale(), liquidTime_, liquidWobble_, looks_)) {
        renderer_.record(cmd, swap, 0, 0, viewport_, false);
        return true;
    }
    // present = false: the flame pass (#480) reads the output and presents instead
    if (present) renderer_.blit(cmd, gpuLiquid_->output(), gpuLiquid_->outW(), gpuLiquid_->outH(), swap, viewport_);
    liquidDrawn_ = true;
    return true;
}

bool App::liquidScreenshot(const std::string& path) {
    if (!liquidDrawn_ || !gpuLiquid_ || !liquidActive()) return false;
    std::vector<std::uint32_t> px;
    if (!gpuLiquid_->readBack(px)) return false;
    return writeBmp(path, px, gpuLiquid_->outW(), gpuLiquid_->outH());
}

void App::setNewLookAndMotion(bool on) {
    newLook_ = on;
    newMotion_ = on;
    smoothPowder_ = calmLiquid_ = on;
}

void App::drawLiquidUi() {
    ImGui::Begin("Look & motion", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    bool both = newLook_ && smoothPowder_ && calmLiquid_;
    if (ImGui::Checkbox("New look + motion (F10 flips old/new)", &both)) setNewLookAndMotion(both);
    ImGui::Checkbox("Smooth liquid surfaces", &newLook_);
    ImGui::SameLine();
    ImGui::Checkbox("Wobble", &liquidWobble_);
    if (!gpuLiquid_) ImGui::TextDisabled("GPU surface pass unavailable: %s", gpuLiquidError_.c_str());
    ImGui::Checkbox("Smooth powder motion", &smoothPowder_);
    ImGui::SameLine();
    ImGui::Checkbox("Calm liquids", &calmLiquid_);
    ImGui::TextDisabled("Off = the phase 1 rules (sand steps, surface flicker)");
    ImGui::End();
}

bool App::writeBmp(const std::string& path, const std::vector<std::uint32_t>& px, int w, int h) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const std::uint32_t dataSize = static_cast<std::uint32_t>(w * h * 4);
    f.write("BM", 2); u32(54 + dataSize); u32(0); u32(54);
    u32(40); u32(static_cast<std::uint32_t>(w)); u32(static_cast<std::uint32_t>(-h)); u16(1); u16(32); u32(0); u32(dataSize); u32(2835); u32(2835); u32(0); u32(0);
    for (std::uint32_t c : px) u32((c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF));
    return static_cast<bool>(f);
}

} // namespace notita
