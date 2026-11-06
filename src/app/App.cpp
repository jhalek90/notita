#include "App.h"

#include "script/ReactionScript.h"
#include "sim/Scenes.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <utility>

namespace notita {

namespace {

std::uint32_t blendOver(std::uint32_t fg, std::uint32_t bg) {
    const std::uint32_t a = fg >> 24;
    auto ch = [&](int shift) {
        const std::uint32_t f = (fg >> shift) & 0xFF, b = (bg >> shift) & 0xFF;
        return ((f * a + b * (255 - a)) / 255) << shift;
    };
    return ch(0) | ch(8) | ch(16) | 0xFF000000u;
}

ImU32 toImColor(std::uint32_t rgba) { return rgba | 0xFF000000u; } // ImU32 is also ABGR in memory

} // namespace

App::App(AppOptions opts) : opts_(std::move(opts)) {}

App::~App() {
    if (device_) SDL_WaitForGPUIdle(device_);
    if (imguiReady_) {
        ImGui_ImplSDL3_Shutdown();
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui::DestroyContext();
    }
    renderer_.shutdown();
    if (device_ && window_) SDL_ReleaseWindowFromGPUDevice(device_, window_);
    if (device_) SDL_DestroyGPUDevice(device_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

bool App::init() {
    // ---- sim ----
    mats_ = MaterialRegistry::loadDefault(&materialErrors_);
    for (const auto& e : materialErrors_) SDL_Log("materials: %s", e.c_str());
    rebuildDisplayPalette();
    buildHeatPalettes();
    reactions_ = std::make_unique<ReactionTable>(mats_);
    {
        std::vector<std::string> errs;
        loadDefaultReactions(*reactions_, &errs);
        for (const auto& e : errs) SDL_Log("reactions: %s", e.c_str());
        materialErrors_.insert(materialErrors_.end(), errs.begin(), errs.end());
    }
    world_ = std::make_unique<World>(0, 0, kWorldChunksW, kWorldChunksH);
    pool_ = std::make_unique<ThreadPool>(opts_.threads);
    sim_ = std::make_unique<Simulator>(*world_, mats_, pool_.get(), opts_.seed, reactions_.get());
    gas_ = std::make_unique<GasGrid>(*world_, mats_, pool_.get(), opts_.seed);
    sim_->setGasExchange(gas_.get());
    sim_->params().ambientRate = 0.0f; // the grid carries the air's heat (and loses some to the ambient)
    gasView_ = opts_.gasView;
    for (int k = 0; k < gas_->speciesCount(); ++k) gasColor_.push_back(mats_[gas_->species(k)].color);
    brush_.material = mats_.idOf("sand");
    brush_.radius = 6;
    loadScene(opts_.scene);
    showTemp_ = opts_.tempView;

    // ---- window + GPU ----
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    window_ = SDL_CreateWindow("Pixel sandbox (working title)", kViewW * 2, kViewH * 2,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!window_) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    // start at 3x (1920x1080) when the screen is big enough, else 2x
    if (const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
        const int factor = (dm->w >= kViewW * 3 + 64 && dm->h >= kViewH * 3 + 96) ? 3 : 2;
        SDL_SetWindowSize(window_, static_cast<int>(kViewW * factor / std::max(scale, 1.0f)),
                          static_cast<int>(kViewH * factor / std::max(scale, 1.0f)));
    }
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window_);

    device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL |
                                      SDL_GPU_SHADERFORMAT_METALLIB,
                                  false, nullptr);
    if (!device_) {
        SDL_Log("SDL_CreateGPUDevice failed: %s", SDL_GetError());
        return false;
    }
    SDL_Log("GPU driver: %s", SDL_GetGPUDeviceDriver(device_));
    if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
        SDL_Log("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
    if (!renderer_.init(device_, kViewW, kViewH)) {
        SDL_Log("renderer init failed: %s", SDL_GetError());
        return false;
    }
    {
        gpuGas_ = std::make_unique<GpuGasSolver>();
        const char* base = SDL_GetBasePath();
        if (gpuGas_->init(device_, std::string(base ? base : "") + "shaders/")) {
            if (!opts_.gasCpu) gas_->setSolver(gpuGas_.get());
            SDL_Log("gas grid %dx%d: GPU solver ready%s", gas_->fields().w, gas_->fields().h, opts_.gasCpu ? " (using the CPU one, --gas-cpu)" : "");
        } else {
            gpuGasError_ = gpuGas_->error();
            SDL_Log("gas grid: GPU solver unavailable (%s), using the CPU solver", gpuGasError_.c_str());
            gpuGas_.reset();
        }
        useGpuGas_ = gpuGas_ && !opts_.gasCpu;
    }

    // ---- Dear ImGui ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini next to the exe
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    ImGui_ImplSDL3_InitForSDLGPU(window_);
    ImGui_ImplSDLGPU3_InitInfo ii{};
    ii.Device = device_;
    ii.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    ii.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ii.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    ii.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
    ImGui_ImplSDLGPU3_Init(&ii);
    imguiReady_ = true;
    return true;
}

void App::rebuildDisplayPalette() {
    displayPalette_.resize(mats_.size());
    const std::uint32_t air = mats_[kEmpty].color | 0xFF000000u;
    for (std::size_t i = 0; i < mats_.size(); ++i)
        for (int s = 0; s < 256; ++s) displayPalette_[i][s] = blendOver(mats_[static_cast<MaterialId>(i)].palette[s], air);
}

namespace {

std::uint32_t rgb(float r, float g, float b, float a = 1.0f) {
    auto c = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

// piecewise-linear colour ramp over (t, r, g, b) stops
std::uint32_t ramp(const float (*stops)[4], int n, float t) {
    if (t <= stops[0][0]) return rgb(stops[0][1], stops[0][2], stops[0][3]);
    for (int i = 1; i < n; ++i)
        if (t <= stops[i][0]) {
            const float f = (t - stops[i - 1][0]) / (stops[i][0] - stops[i - 1][0]);
            auto mix = [&](int k) { return stops[i - 1][k] + (stops[i][k] - stops[i - 1][k]) * f; };
            return rgb(mix(1), mix(2), mix(3));
        }
    return rgb(stops[n - 1][1], stops[n - 1][2], stops[n - 1][3]);
}

std::uint32_t mixColor(std::uint32_t a, std::uint32_t b, std::uint32_t wb) { // wb: 0..255 weight of b
    auto ch = [&](int s) {
        const std::uint32_t x = (a >> s) & 0xFF, y = (b >> s) & 0xFF;
        return ((x * (255 - wb) + y * wb) / 255) << s;
    };
    return ch(0) | ch(8) | ch(16) | 0xFF000000u;
}

} // namespace

void App::buildHeatPalettes() {
    // thermal-camera style: cold blues, ambient near black, then red, orange, yellow, white
    static const float temp[][4] = {
        {-100, 0.55f, 0.85f, 1.00f}, {-20, 0.10f, 0.35f, 0.85f}, {10, 0.05f, 0.08f, 0.25f}, {20, 0.06f, 0.06f, 0.08f},
        {45, 0.25f, 0.05f, 0.35f},  {100, 0.65f, 0.05f, 0.35f}, {300, 0.90f, 0.20f, 0.05f}, {700, 1.00f, 0.60f, 0.05f},
        {1200, 1.00f, 0.95f, 0.35f}, {1800, 1.00f, 1.00f, 1.00f},
    };
    for (std::size_t i = 0; i < tempPalette_.size(); ++i)
        tempPalette_[i] = ramp(temp, static_cast<int>(std::size(temp)), -128.0f + 2.0f * static_cast<float>(i));
    // incandescence: dull red from ~450 C to white-yellow past 1500 C
    static const float glow[][4] = {
        {400, 0.45f, 0.02f, 0.00f}, {650, 0.85f, 0.15f, 0.02f}, {950, 1.00f, 0.50f, 0.08f}, {1300, 1.00f, 0.85f, 0.40f},
        {1675, 1.00f, 1.00f, 0.85f},
    };
    for (std::size_t i = 0; i < glowColor_.size(); ++i) {
        const float t = 400.0f + 5.0f * static_cast<float>(i);
        const float a = std::clamp((t - 420.0f) / 700.0f, 0.0f, 0.9f);
        glowColor_[i] = (ramp(glow, static_cast<int>(std::size(glow)), t) & 0x00FFFFFFu) |
                        (static_cast<std::uint32_t>(a * 255.0f) << 24);
    }
}

void App::loadScene(const std::string& name) {
    if (name == "churn") scenes::churn(*world_, mats_, opts_.seed);
    else if (name == "forge") scenes::forge(*world_, mats_, opts_.seed);
    else if (name == "thermal") scenes::thermalChurn(*world_, mats_, opts_.seed);
    else if (name == "gas") scenes::gasLab(*world_, mats_, opts_.seed);
    else if (name == "empty") world_->clear();
    else scenes::demo(*world_, mats_, opts_.seed);
    if (gas_) gas_->clear();
    camX_ = static_cast<float>(world_->minX() + (world_->widthPx() - kViewW) / 2);
    camY_ = static_cast<float>(world_->minY() + world_->heightPx() - kViewH);
}

bool App::screenToWorld(float sx, float sy, int& wx, int& wy) const {
    if (viewport_.w <= 0) return false;
    const float vx = (sx - viewport_.x) / viewport_.scale, vy = (sy - viewport_.y) / viewport_.scale;
    wx = static_cast<int>(std::floor(vx)) + static_cast<int>(camX_);
    wy = static_cast<int>(std::floor(vy)) + static_cast<int>(camY_);
    return vx >= 0 && vy >= 0 && vx < kViewW && vy < kViewH;
}

void App::handleEvent(const SDL_Event& e) {
    ImGuiIO& io = ImGui::GetIO();
    const float density = SDL_GetWindowPixelDensity(window_);
    switch (e.type) {
    case SDL_EVENT_QUIT: running_ = false; break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: running_ = false; break;
    case SDL_EVENT_MOUSE_MOTION:
        mouseX_ = e.motion.x * density;
        mouseY_ = e.motion.y * density;
        if (panning_) {
            camX_ -= e.motion.xrel * density / viewport_.scale;
            camY_ -= e.motion.yrel * density / viewport_.scale;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (io.WantCaptureMouse) break;
        if (e.button.button == SDL_BUTTON_LEFT) painting_ = true;
        if (e.button.button == SDL_BUTTON_RIGHT) erasing_ = true;
        if (e.button.button == SDL_BUTTON_MIDDLE) panning_ = true;
        screenToWorld(mouseX_, mouseY_, lastWX_, lastWY_);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button == SDL_BUTTON_LEFT) painting_ = false;
        if (e.button.button == SDL_BUTTON_RIGHT) erasing_ = false;
        if (e.button.button == SDL_BUTTON_MIDDLE) panning_ = false;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (io.WantCaptureMouse) break;
        brush_.radius = std::clamp(brush_.radius + (e.wheel.y > 0 ? 1 : -1) * std::max(1, brush_.radius / 6), 0, 64);
        break;
    case SDL_EVENT_KEY_DOWN: {
        if (io.WantCaptureKeyboard) break;
        const SDL_Keycode k = e.key.key;
        if (k == SDLK_SPACE) paused_ = !paused_;
        else if (k == SDLK_PERIOD || k == SDLK_N) { paused_ = true; ++stepRequests_; }
        else if (k == SDLK_TAB || k == SDLK_F1) showUi_ = !showUi_;
        else if (k == SDLK_F3) showChunks_ = !showChunks_;
        else if (k == SDLK_F4) showDirty_ = !showDirty_;
        else if (k == SDLK_F5) showTemp_ = !showTemp_;
        else if (k == SDLK_F6) gasView_ = (gasView_ + 1) % 3;
        else if (k == SDLK_G) gas_->params().enabled = !gas_->params().enabled;
        else if (k == SDLK_V) loadScene("gas");
        else if (k == SDLK_X) {
            int bx, by;
            if (screenToWorld(mouseX_, mouseY_, bx, by)) gas_->explode(bx, by, blastRadius_, blastPower_);
        }
        else if (k == SDLK_H) brush_.mode = BrushMode::Heat;
        else if (k == SDLK_J) brush_.mode = BrushMode::Cool;
        else if (k == SDLK_P) brush_.mode = BrushMode::Place;
        else if (k == SDLK_F) loadScene("forge");
        else if (k == SDLK_LEFTBRACKET) brush_.radius = std::max(0, brush_.radius - 1);
        else if (k == SDLK_RIGHTBRACKET) brush_.radius = std::min(64, brush_.radius + 1);
        else if (k == SDLK_R) loadScene("demo");
        else if (k == SDLK_B) loadScene("churn");
        else if (k == SDLK_C) world_->clear();
        else if (k == SDLK_ESCAPE) running_ = false;
        else if (k >= SDLK_1 && k <= SDLK_9) {
            const auto id = static_cast<std::size_t>(k - SDLK_1 + 1);
            if (id < mats_.size()) brush_.material = static_cast<MaterialId>(id);
        }
        break;
    }
    default: break;
    }
}

void App::updateInput(float dt) {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (!ImGui::GetIO().WantCaptureKeyboard) {
        const float pan = 360.0f * dt * (keys[SDL_SCANCODE_LSHIFT] ? 3.0f : 1.0f);
        if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) camX_ -= pan;
        if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) camX_ += pan;
        if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) camY_ -= pan;
        if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) camY_ += pan;
    }
    camX_ = std::clamp(camX_, static_cast<float>(world_->minX() - kViewW / 2),
                       static_cast<float>(world_->minX() + world_->widthPx() - kViewW / 2));
    camY_ = std::clamp(camY_, static_cast<float>(world_->minY() - kViewH / 2),
                       static_cast<float>(world_->minY() + world_->heightPx() - kViewH / 2));

    // brushes act between ticks on the main thread, so they never race the sim
    if (painting_ || erasing_) {
        int wx, wy;
        screenToWorld(mouseX_, mouseY_, wx, wy);
        Brush b = brush_;
        const bool ctrl = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
        if (ctrl) b.mode = erasing_ ? BrushMode::Cool : BrushMode::Heat; // Ctrl: LMB heats, RMB cools
        else if (erasing_) b.mode = BrushMode::Erase;
        else if (b.mode == BrushMode::Heat || b.mode == BrushMode::Cool) {} // heat brush picked in the panel
        else if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) b.mode = BrushMode::Replace;
        // heat brushes are per second, not per dab, so they don't depend on the frame rate
        if (b.mode == BrushMode::Heat || b.mode == BrushMode::Cool) b.heat = brush_.heat * 60.0f * dt;
        strokeBrush(*world_, mats_, b, lastWX_, lastWY_, wx, wy, brushRng_);
        lastWX_ = wx;
        lastWY_ = wy;
    }
}

void App::stepSim(double frameDt) {
    ticksThisFrame_ = 0;
    if (paused_) {
        accumulator_ = 0.0;
        while (stepRequests_ > 0) { tickSim(); --stepRequests_; ++ticksThisFrame_; }
    } else {
        accumulator_ += std::min(frameDt, 0.25) * speed_;
        constexpr int kMaxTicksPerFrame = 4; // don't spiral if a frame was slow
        while (accumulator_ >= kTickDt && ticksThisFrame_ < kMaxTicksPerFrame) {
            tickSim();
            accumulator_ -= kTickDt;
            ++ticksThisFrame_;
        }
        if (ticksThisFrame_ == kMaxTicksPerFrame) accumulator_ = 0.0;
        stepRequests_ = 0;
    }
    if (ticksThisFrame_ > 0) {
        tickHistory_[static_cast<std::size_t>(tickHistoryPos_)] = static_cast<float>(sim_->stats().tickMs);
        tickHistoryPos_ = (tickHistoryPos_ + 1) % static_cast<int>(tickHistory_.size());
    }
}

void App::tickSim() {
    sim_->tick();
    sim_->params().ambientRate = gas_->params().enabled ? 0.0f : sim_->params().ambientRate;
    gas_->step();
}

void App::drawWorld() {
    auto& px = renderer_.pixels();
    const std::uint32_t outside = 0xFF0A0606u;
    std::fill(px.begin(), px.end(), outside);
    const int cx0 = static_cast<int>(camX_), cy0 = static_cast<int>(camY_);
    // walk the chunks that overlap the view and copy rows (no per-pixel lookups)
    for (int ccy = cy0 >> kChunkShift; ccy <= (cy0 + kViewH - 1) >> kChunkShift; ++ccy)
        for (int ccx = cx0 >> kChunkShift; ccx <= (cx0 + kViewW - 1) >> kChunkShift; ++ccx) {
            const Chunk* ch = world_->chunk(ccx, ccy);
            if (!ch) continue;
            const int ox = ccx * kChunkSize, oy = ccy * kChunkSize;
            const int x0 = std::max(ox, cx0), x1 = std::min(ox + kChunkSize, cx0 + kViewW);
            const int y0 = std::max(oy, cy0), y1 = std::min(oy + kChunkSize, cy0 + kViewH);
            for (int y = y0; y < y1; ++y) {
                std::uint32_t* row = &px[static_cast<std::size_t>(y - cy0) * kViewW];
                const Cell* cells = &ch->at(0, y - oy);
                if (showTemp_) {
                    // temperature view: the heat colour, with a little of the material showing through
                    for (int x = x0; x < x1; ++x) {
                        const Cell& c = cells[x - ox];
                        const int ti = std::clamp(static_cast<int>((c.temperature + 128.0f) * 0.5f), 0, 1023);
                        const std::uint32_t base = displayPalette_[c.material][c.shade];
                        row[x - cx0] = mixColor(base, tempPalette_[static_cast<std::size_t>(ti)], c.material == kEmpty ? 235 : 190);
                    }
                } else if (glow_) {
                    for (int x = x0; x < x1; ++x) {
                        const Cell& c = cells[x - ox];
                        std::uint32_t col = displayPalette_[c.material][c.shade];
                        if (c.temperature > 420.0f && !mats_[c.material].isFlame()) {
                            const int gi = std::min(static_cast<int>((c.temperature - 400.0f) * 0.2f), 255);
                            const std::uint32_t g = glowColor_[static_cast<std::size_t>(gi)];
                            col = mixColor(col, g | 0xFF000000u, g >> 24);
                        }
                        row[x - cx0] = col;
                    }
                } else {
                    for (int x = x0; x < x1; ++x) {
                        const Cell& c = cells[x - ox];
                        row[x - cx0] = displayPalette_[c.material][c.shade];
                    }
                }
            }
        }
    drawGas();
}

void App::drawGas() {
    if (!showGas_ || !gas_) return;
    const GasFields& f = gas_->fields();
    if (f.w <= 0) return;
    auto& px = renderer_.pixels();
    const int cx0 = static_cast<int>(camX_), cy0 = static_cast<int>(camY_);
    const int gx0 = gas_->originX(), gy0 = gas_->originY();
    const std::size_t n = f.cells();
    for (int y = 0; y < kViewH; ++y) {
        // bilinear in y and x over cell centres, so the gas has no 4x4 blocks
        const float gy = (static_cast<float>(cy0 + y - gy0) + 0.5f) / kGasCell - 0.5f;
        if (gy < -0.5f || gy > static_cast<float>(f.h) - 0.5f) continue;
        const int j0 = std::clamp(static_cast<int>(std::floor(gy)), 0, f.h - 1), j1 = std::min(j0 + 1, f.h - 1);
        const float fy = std::clamp(gy - static_cast<float>(j0), 0.0f, 1.0f);
        std::uint32_t* row = &px[static_cast<std::size_t>(y) * kViewW];
        for (int x = 0; x < kViewW; ++x) {
            const float gx = (static_cast<float>(cx0 + x - gx0) + 0.5f) / kGasCell - 0.5f;
            if (gx < -0.5f || gx > static_cast<float>(f.w) - 0.5f) continue;
            const int i0 = std::clamp(static_cast<int>(std::floor(gx)), 0, f.w - 1), i1 = std::min(i0 + 1, f.w - 1);
            const float fx = std::clamp(gx - static_cast<float>(i0), 0.0f, 1.0f);
            const int c00 = j0 * f.w + i0, c10 = j0 * f.w + i1, c01 = j1 * f.w + i0, c11 = j1 * f.w + i1;
            const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
            auto lerp = [&](auto get) { return get(c00) * w00 + get(c10) * w10 + get(c01) * w01 + get(c11) * w11; };
            std::uint32_t& out = row[x];
            if (gasView_ == 0) {
                // each gas tints the pixel by its concentration (units per pixel)
                for (int k = 0; k < f.species; ++k) {
                    const float conc = lerp([&](int c) {
                        return static_cast<float>(f.mass[static_cast<std::size_t>(k) * n + c]) / (kGasMassOne * kGasCellPx);
                    });
                    if (conc < 0.004f) continue;
                    const std::uint32_t col = gasColor_[static_cast<std::size_t>(k)];
                    const float a = std::min(1.0f, conc * 1.6f) * (0.35f + 0.65f * static_cast<float>(col >> 24) / 255.0f);
                    out = mixColor(out, col | 0xFF000000u, static_cast<std::uint32_t>(a * 230.0f));
                }
            } else if (gasView_ == 1) {
                // velocity: hue by direction, brightness by speed
                const float u = lerp([&](int c) { return f.uc(c % f.w, c / f.w); });
                const float v = lerp([&](int c) { return f.vc(c % f.w, c / f.w); });
                const float sp = std::sqrt(u * u + v * v);
                if (sp < 0.002f) continue;
                const float a = std::atan2(v, u);
                const auto ch = [&](float off) {
                    return static_cast<std::uint32_t>(std::clamp(0.5f + 0.5f * std::cos(a - off), 0.0f, 1.0f) * 255.0f);
                };
                const std::uint32_t col = 0xFF000000u | ch(0.0f) | (ch(2.094f) << 8) | (ch(4.189f) << 16);
                out = mixColor(out, col, static_cast<std::uint32_t>(std::min(1.0f, sp * 3.0f) * 220.0f));
            } else {
                // pressure: red above zero, blue below
                const float p = lerp([&](int c) { return f.open[c] > 0.0f ? f.p[c] : 0.0f; });
                const float a = std::min(1.0f, std::abs(p) * 0.5f);
                if (a < 0.01f) continue;
                out = mixColor(out, p > 0 ? 0xFF3040F0u : 0xFFF07030u, static_cast<std::uint32_t>(a * 220.0f));
            }
        }
    }
}

void App::drawOverlays(const Viewport& vp) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float inv = 1.0f / SDL_GetWindowPixelDensity(window_); // swapchain px -> ImGui units
    auto toScreen = [&](float wx, float wy) {
        return ImVec2((vp.x + (wx - std::floor(camX_)) * vp.scale) * inv, (vp.y + (wy - std::floor(camY_)) * vp.scale) * inv);
    };
    dl->PushClipRect(ImVec2(vp.x * inv, vp.y * inv), ImVec2((vp.x + vp.w) * inv, (vp.y + vp.h) * inv), true);
    if (showChunks_ || showDirty_) {
        for (const Chunk* ch : world_->chunks()) {
            const float ox = static_cast<float>(ch->originX()), oy = static_cast<float>(ch->originY());
            if (ox + kChunkSize < camX_ || oy + kChunkSize < camY_ || ox > camX_ + kViewW || oy > camY_ + kViewH) continue;
            if (showChunks_)
                dl->AddRect(toScreen(ox, oy), toScreen(ox + kChunkSize, oy + kChunkSize),
                            ch->active() ? IM_COL32(90, 220, 120, 110) : IM_COL32(255, 255, 255, 28));
            if (showDirty_ && ch->active()) {
                const DirtyRect& r = ch->rect;
                dl->AddRect(toScreen(ox + r.x0, oy + r.y0), toScreen(ox + r.x1 + 1, oy + r.y1 + 1), IM_COL32(255, 90, 60, 160));
            }
        }
    }
    // brush outline
    int wx, wy;
    if (screenToWorld(mouseX_, mouseY_, wx, wy) && !ImGui::GetIO().WantCaptureMouse) {
        const float r = static_cast<float>(brush_.radius) + 0.5f;
        const ImU32 col = IM_COL32(255, 255, 255, 140);
        const ImVec2 c = toScreen(static_cast<float>(wx) + 0.5f, static_cast<float>(wy) + 0.5f);
        if (brush_.shape == BrushShape::Circle) dl->AddCircle(c, r * vp.scale * inv, col, 32);
        else dl->AddRect(toScreen(wx - r + 0.5f, wy - r + 0.5f), toScreen(wx + r + 0.5f, wy + r + 0.5f), col);
    }
    dl->PopClipRect();
}

void App::drawUi() {
    if (!showUi_) return;
    const SimStats& st = sim_->stats();

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::Begin("Sandbox", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushItemWidth(ImGui::GetFontSize() * 11.0f);
    ImGui::SeparatorText("Material");
    ImGui::BeginChild("##materials", ImVec2(ImGui::GetFontSize() * 15.0f, ImGui::GetFontSize() * 14.0f));
    for (std::size_t i = 1; i < mats_.size(); ++i) {
        const Material& m = mats_[static_cast<MaterialId>(i)];
        ImGui::PushID(static_cast<int>(i));
        const ImU32 c = toImColor(displayPalette_[i][160]);
        ImGui::ColorButton("##c", ImGui::ColorConvertU32ToFloat4(c), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
        ImGui::SameLine();
        char label[96];
        std::snprintf(label, sizeof label, i <= 9 ? "%s  [%zu]" : "%s", m.label.c_str(), i);
        if (ImGui::Selectable(label, brush_.material == m.id)) brush_.material = m.id;
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s, density %.1f%s", kindName(m.kind), m.density,
                        m.kind == MaterialKind::Liquid ? (m.viscosity > 0.5f ? ", viscous" : "") : "");
            ImGui::Text("conductivity %.2f, heat capacity %.2f, placed at %.0f C", m.conductivity, m.heatCapacity, m.spawnTemp);
            if (m.hot.valid) ImGui::Text("above %.0f C -> %s", m.hot.point, mats_[m.hot.into].label.c_str());
            if (m.cold.valid) ImGui::Text("below %.0f C -> %s", m.cold.point, mats_[m.cold.into].label.c_str());
            if (m.flammable()) ImGui::Text("ignites at %.0f C, burns into %s", m.ignitePoint, mats_[m.burnsInto.into].label.c_str());
            for (const auto& e : reactions_->of(m.id)) {
                const Reaction& r = reactions_->reactions()[e.index];
                ImGui::Text("+ %s -> %s + %s", mats_[e.partner].label.c_str(), mats_[e.swapped ? r.outB : r.outA].label.c_str(),
                            mats_[e.swapped ? r.outA : r.outB].label.c_str());
            }
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SeparatorText("Brush");
    ImGui::SliderInt("Size", &brush_.radius, 0, 64);
    int shape = brush_.shape == BrushShape::Circle ? 0 : 1;
    if (ImGui::Combo("Shape", &shape, "Circle\0Square\0")) brush_.shape = shape == 0 ? BrushShape::Circle : BrushShape::Square;
    int mode = brush_.mode == BrushMode::Place ? 0 : brush_.mode == BrushMode::Replace ? 1 : brush_.mode == BrushMode::Heat ? 2 : 3;
    if (ImGui::Combo("Mode", &mode, "Fill empty\0Replace all\0Heat (H)\0Cool (J)\0")) {
        const BrushMode modes[] = {BrushMode::Place, BrushMode::Replace, BrushMode::Heat, BrushMode::Cool};
        brush_.mode = modes[mode];
    }
    if (brush_.mode == BrushMode::Heat || brush_.mode == BrushMode::Cool)
        ImGui::SliderFloat("Heat", &brush_.heat, 1.0f, 200.0f, "%.0f C per tick");
    ImGui::SliderFloat("Spray", &brush_.density, 0.05f, 1.0f, "%.2f");
    ImGui::TextDisabled("LMB paint, RMB erase, Shift = replace");
    ImGui::TextDisabled("Ctrl+LMB heat, Ctrl+RMB cool, P paint");
    ImGui::TextDisabled("Wheel / [ ] size, MMB / WASD pan");

    ImGui::SeparatorText("Simulation");
    ImGui::Checkbox("Paused (Space)", &paused_);
    ImGui::SameLine();
    if (ImGui::Button("Step (.)")) { paused_ = true; ++stepRequests_; }
    ImGui::SliderFloat("Speed", &speed_, 0.1f, 4.0f, "%.2fx");
    SimParams& sp = sim_->params();
    ImGui::Checkbox("Heat", &sp.heat);
    ImGui::SameLine();
    ImGui::Checkbox("Reactions", &sp.reactions);
    ImGui::SliderFloat("Conduction", &sp.conduction, 0.05f, 1.0f, "%.2f");
    ImGui::SliderFloat("Air cooling", &sp.ambientRate, 0.0f, 0.03f, "%.4f");
    ImGui::Checkbox("Temperature view (F5)", &showTemp_);
    ImGui::SameLine();
    ImGui::Checkbox("Glow", &glow_);
    if (ImGui::Button("Demo (R)")) loadScene("demo");
    ImGui::SameLine();
    if (ImGui::Button("Forge (F)")) loadScene("forge");
    ImGui::SameLine();
    if (ImGui::Button("Full churn (B)")) loadScene("churn");
    ImGui::SameLine();
    if (ImGui::Button("Gas lab (V)")) loadScene("gas");
    ImGui::SameLine();
    if (ImGui::Button("Clear (C)")) world_->clear();
    ImGui::PopItemWidth();
    ImGui::End();

    drawGasUi();

    // debug overlay
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10, 10), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("Debug", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    float avg = 0, peak = 0;
    for (float v : tickHistory_) { avg += v; peak = std::max(peak, v); }
    avg /= static_cast<float>(tickHistory_.size());
    ImGui::Text("%.0f fps  frame %.2f ms", io.Framerate, frameMs_);
    ImGui::Text("tick %.3f ms  (avg %.3f, max %.3f)", st.tickMs, avg, peak);
    ImGui::PlotLines("##ticks", tickHistory_.data(), static_cast<int>(tickHistory_.size()), tickHistoryPos_, nullptr, 0.0f,
                     std::max(2.0f, peak * 1.2f), ImVec2(220, 36));
    ImGui::Text("chunks %d / %d moving, %d heat", st.activeChunks, st.totalChunks, st.heatChunks);
    ImGui::Text("events %llu (changes, burns, reactions)", static_cast<unsigned long long>(st.events));
    ImGui::Text("cells moved/visited %llu", static_cast<unsigned long long>(st.cellsProcessed));
    ImGui::Text("tick #%llu  threads %d  seed %llu", static_cast<unsigned long long>(st.tick), pool_->threadCount(),
                static_cast<unsigned long long>(sim_->seed()));
    ImGui::Text("%s", paused_ ? "PAUSED" : (speed_ != 1.0f ? "speed changed" : "60 Hz fixed tick"));
    ImGui::Checkbox("Chunks (F3)", &showChunks_);
    ImGui::SameLine();
    ImGui::Checkbox("Dirty rects (F4)", &showDirty_);
    int wx, wy;
    if (screenToWorld(mouseX_, mouseY_, wx, wy)) {
        if (const Cell* c = world_->cell(wx, wy))
            ImGui::Text("(%d, %d) %s%s  %.1f C  vy %.2f", wx, wy, mats_[c->material].label.c_str(),
                        (c->flags & kFlagBurning) ? " (burning)" : "", c->temperature, c->vy / 256.0);
    }
    if (!materialErrors_.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%zu material/reaction error(s), see log", materialErrors_.size());
    ImGui::End();
}

void App::drawGasUi() {
    GasGridParams& gp = gas_->params();
    GasSolveParams& sp = gas_->solveParams();
    const GasGridStats& gs = gas_->stats();
    ImGui::SetNextWindowPos(ImVec2(10, 640), ImGuiCond_FirstUseEver);
    ImGui::Begin("Gas grid", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
    ImGui::Checkbox("Gas grid on (G)", &gp.enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Show gas", &showGas_);
    if (gpuGas_) {
        if (ImGui::Checkbox("GPU solver", &useGpuGas_)) gas_->setSolver(useGpuGas_ ? gpuGas_.get() : nullptr);
    } else {
        ImGui::TextDisabled("GPU solver unavailable: %s", gpuGasError_.c_str());
    }
    ImGui::Combo("View (F6)", &gasView_, "Gas\0Velocity\0Pressure\0");
    ImGui::SliderInt("Pressure iterations", &sp.pressureIters, 4, 120);
    ImGui::SliderFloat("Heat buoyancy", &sp.buoyancyHeat, 0.0f, 0.004f, "%.4f");
    ImGui::SliderFloat("Gas buoyancy", &sp.buoyancyDensity, 0.0f, 0.05f, "%.3f");
    ImGui::SliderFloat("Vorticity", &sp.vorticity, 0.0f, 0.5f, "%.2f");
    ImGui::SliderFloat("Damping", &sp.damping, 0.95f, 1.0f, "%.3f");
    ImGui::SliderFloat("Air exchange", &gp.airExchange, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Loss to ambient", &gp.ambientLoss, 0.0f, 0.02f, "%.4f");
    ImGui::SliderFloat("Push on pixels", &gp.push, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("Blast power", &blastPower_, 0.2f, 4.0f, "%.1f");
    ImGui::SliderInt("Blast radius", &blastRadius_, 4, 40);
    ImGui::TextDisabled("X: blast at the cursor");
    double grams = static_cast<double>(gas_->massFixed()) / kGasMassOne;
    ImGui::Text("%s solver %.2f ms (%s), coupling %.2f ms", gas_->solver().name(), gs.solveMs,
                gpuGas_ && useGpuGas_ ? "upload+kernels+download" : "threads", gs.coupleMs);
    ImGui::Text("grid %dx%d, %.0f mass units of gas", gas_->fields().w, gas_->fields().h, grams);
    ImGui::Text("max speed %.2f px/tick, max |p| %.2f", gs.maxSpeed * kGasCell, gs.maxPressure);
    ImGui::Text("in %lld, condensed %lld, burnt %lld, pushed %d px, %d shoves", gs.absorbed, gs.condensed, gs.burned, gs.pushed, gs.shoved);
    int wx, wy;
    if (screenToWorld(mouseX_, mouseY_, wx, wy)) {
        const int i = (wx - gas_->originX()) / kGasCell, j = (wy - gas_->originY()) / kGasCell;
        const GasFields& f = gas_->fields();
        if (i >= 0 && j >= 0 && i < f.w && j < f.h) {
            const int c = j * f.w + i;
            ImGui::Text("cell (%d, %d): %.0f C, p %.3f, v (%.2f, %.2f)", i, j, gas_->temperature(c), f.p[c], f.uc(i, j), f.vc(i, j));
            for (int k = 0; k < f.species; ++k)
                if (f.m(k, c) > 0) ImGui::Text("  %s %.2f", mats_[gas_->species(k)].label.c_str(), static_cast<double>(f.m(k, c)) / kGasMassOne);
        }
    }
    ImGui::PopItemWidth();
    ImGui::End();
}

int App::gasSelfTest() {
    // The same grid state through both solvers: mass must match exactly
    // (both keep it), velocities and heat to float precision.
    if (!gpuGas_) {
        SDL_Log("gas self-test: no GPU solver (%s)", gpuGasError_.c_str());
        return 2;
    }
    loadScene("gas");
    gas_->setSolver(nullptr);
    for (int t = 0; t < 240; ++t) tickSim(); // get some gas moving on the CPU
    gas_->explode(350, world_->minY() + world_->heightPx() - 80, 16, 2.0f);
    tickSim();
    GasFields a = gas_->fields();
    GasFields b = a;
    CpuGasSolver cpu(pool_.get());
    const GasSolveParams sp = gas_->solveParams();
    auto total = [](const GasFields& f) { long long s = 0; for (int m : f.mass) s += m; return s; };
    const long long m0 = total(a);
    double gpuMs = 0;
    const int steps = 30;
    for (int s = 0; s < steps; ++s) {
        cpu.solve(a, sp);
        gpuGas_->solve(b, sp);
        gpuMs += gpuGas_->lastMs();
    }
    double du = 0, dh = 0, maxU = 0, maxH = 0;
    for (std::size_t i = 0; i < a.u.size(); ++i) { du = std::max(du, static_cast<double>(std::abs(a.u[i] - b.u[i]))); maxU = std::max(maxU, static_cast<double>(std::abs(a.u[i]))); }
    for (std::size_t i = 0; i < a.heat.size(); ++i) { dh = std::max(dh, static_cast<double>(std::abs(a.heat[i] - b.heat[i]))); maxH = std::max(maxH, static_cast<double>(std::abs(a.heat[i]))); }
    long long dm = 0;
    for (std::size_t i = 0; i < a.mass.size(); ++i) dm = std::max(dm, static_cast<long long>(std::abs(a.mass[i] - b.mass[i])));
    const bool massKept = total(a) == m0 && total(b) == m0;
    SDL_Log("gas self-test, grid %dx%d, %d species, %d steps from the same state:", a.w, a.h, a.species, steps);
    SDL_Log("  mass total cpu %lld, gpu %lld, start %lld -> %s", total(a), total(b), m0, massKept ? "kept exactly" : "CHANGED");
    SDL_Log("  max |u cpu - u gpu| %.2e (max |u| %.3f), max heat diff %.2e (max %.1f), max cell mass diff %lld", du, maxU, dh, maxH, dm);
    SDL_Log("  gpu solve %.3f ms per step (upload + %d kernels + download)", gpuMs / steps, 7 + sp.pressureIters);
    const bool close = du <= 0.02 * std::max(1.0, maxU) && dh <= 0.02 * std::max(1.0, maxH);
    SDL_Log("  %s", massKept && close ? "PASS" : "FAIL");
    return massKept && close ? 0 : 1;
}

bool App::writeScreenshot(const std::string& path) const {
    // 32-bit BMP of the sim view (what the renderer uploads)
    const auto& px = renderer_.pixels();
    const int w = kViewW, h = kViewH;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const std::uint32_t dataSize = static_cast<std::uint32_t>(w * h * 4);
    f.write("BM", 2); u32(54 + dataSize); u32(0); u32(54);
    u32(40); u32(static_cast<std::uint32_t>(w)); u32(static_cast<std::uint32_t>(-h)); u16(1); u16(32); u32(0); u32(dataSize); u32(2835); u32(2835); u32(0); u32(0);
    for (std::uint32_t c : px) {
        const std::uint32_t bgra = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
        u32(bgra);
    }
    return static_cast<bool>(f);
}

int App::run() {
    auto last = std::chrono::steady_clock::now();
    int frame = 0;
    while (running_) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            handleEvent(e);
        }
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        frameMs_ = dt * 1000.0;

        if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        updateInput(static_cast<float>(dt));
        stepSim(dt);
        drawWorld();

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        SDL_GPUTexture* swap = nullptr;
        Uint32 sw = 0, sh = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swap, &sw, &sh)) swap = nullptr;
        if (swap) viewport_ = renderer_.fit(sw, sh);

        drawOverlays(viewport_);
        drawUi();
        ImGui::Render();
        ImDrawData* dd = ImGui::GetDrawData();

        if (swap) {
            renderer_.record(cmd, swap, sw, sh, viewport_);
            ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd);
            SDL_GPUColorTargetInfo target{};
            target.texture = swap;
            target.load_op = SDL_GPU_LOADOP_LOAD;
            target.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd, pass);
            SDL_EndGPURenderPass(pass);
        }
        SDL_SubmitGPUCommandBuffer(cmd);

        if (opts_.frames > 0 && ++frame >= opts_.frames) {
            if (!opts_.screenshot.empty() && !writeScreenshot(opts_.screenshot))
                SDL_Log("could not write %s", opts_.screenshot.c_str());
            SDL_Log("ran %d frames, last tick %.3f ms, %d/%d chunks active", frame, sim_->stats().tickMs,
                    sim_->stats().activeChunks, sim_->stats().totalChunks);
            running_ = false;
        }
    }
    return 0;
}

} // namespace notita
