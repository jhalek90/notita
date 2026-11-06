#include "App.h"

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
    world_ = std::make_unique<World>(0, 0, kWorldChunksW, kWorldChunksH);
    pool_ = std::make_unique<ThreadPool>(opts_.threads);
    sim_ = std::make_unique<Simulator>(*world_, mats_, pool_.get(), opts_.seed);
    brush_.material = mats_.idOf("sand");
    brush_.radius = 6;
    loadScene(opts_.scene);

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

void App::loadScene(const std::string& name) {
    if (name == "churn") scenes::churn(*world_, mats_, opts_.seed);
    else if (name == "empty") world_->clear();
    else scenes::demo(*world_, mats_, opts_.seed);
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
        if (erasing_) b.mode = BrushMode::Erase;
        else if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) b.mode = BrushMode::Replace;
        strokeBrush(*world_, mats_, b, lastWX_, lastWY_, wx, wy, brushRng_);
        lastWX_ = wx;
        lastWY_ = wy;
    }
}

void App::stepSim(double frameDt) {
    ticksThisFrame_ = 0;
    if (paused_) {
        accumulator_ = 0.0;
        while (stepRequests_ > 0) { sim_->tick(); --stepRequests_; ++ticksThisFrame_; }
    } else {
        accumulator_ += std::min(frameDt, 0.25) * speed_;
        constexpr int kMaxTicksPerFrame = 4; // don't spiral if a frame was slow
        while (accumulator_ >= kTickDt && ticksThisFrame_ < kMaxTicksPerFrame) {
            sim_->tick();
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
                for (int x = x0; x < x1; ++x) {
                    const Cell& c = cells[x - ox];
                    row[x - cx0] = displayPalette_[c.material][c.shade];
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
    for (std::size_t i = 1; i < mats_.size(); ++i) {
        const Material& m = mats_[static_cast<MaterialId>(i)];
        ImGui::PushID(static_cast<int>(i));
        const ImU32 c = toImColor(displayPalette_[i][160]);
        ImGui::ColorButton("##c", ImGui::ColorConvertU32ToFloat4(c), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
        ImGui::SameLine();
        char label[96];
        std::snprintf(label, sizeof label, i <= 9 ? "%s  [%zu]" : "%s", m.label.c_str(), i);
        if (ImGui::Selectable(label, brush_.material == m.id)) brush_.material = m.id;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s, density %.1f%s", kindName(m.kind), m.density,
                              m.kind == MaterialKind::Liquid ? (m.viscosity > 0.5f ? ", viscous" : "") : "");
        ImGui::PopID();
    }
    ImGui::SeparatorText("Brush");
    ImGui::SliderInt("Size", &brush_.radius, 0, 64);
    int shape = brush_.shape == BrushShape::Circle ? 0 : 1;
    if (ImGui::Combo("Shape", &shape, "Circle\0Square\0")) brush_.shape = shape == 0 ? BrushShape::Circle : BrushShape::Square;
    int mode = brush_.mode == BrushMode::Place ? 0 : 1;
    if (ImGui::Combo("Mode", &mode, "Fill empty\0Replace all\0")) brush_.mode = mode == 0 ? BrushMode::Place : BrushMode::Replace;
    ImGui::SliderFloat("Spray", &brush_.density, 0.05f, 1.0f, "%.2f");
    ImGui::TextDisabled("LMB paint, RMB erase, Shift = replace");
    ImGui::TextDisabled("Wheel / [ ] size, MMB / WASD pan");

    ImGui::SeparatorText("Simulation");
    ImGui::Checkbox("Paused (Space)", &paused_);
    ImGui::SameLine();
    if (ImGui::Button("Step (.)")) { paused_ = true; ++stepRequests_; }
    ImGui::SliderFloat("Speed", &speed_, 0.1f, 4.0f, "%.2fx");
    if (ImGui::Button("Demo (R)")) loadScene("demo");
    ImGui::SameLine();
    if (ImGui::Button("Full churn (B)")) loadScene("churn");
    ImGui::SameLine();
    if (ImGui::Button("Clear (C)")) world_->clear();
    ImGui::PopItemWidth();
    ImGui::End();

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
    ImGui::Text("chunks %d / %d active", st.activeChunks, st.totalChunks);
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
            ImGui::Text("(%d, %d) %s  vy %.2f", wx, wy, mats_[c->material].label.c_str(), c->vy / 256.0);
    }
    if (!materialErrors_.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%zu material file error(s), see log", materialErrors_.size());
    ImGui::End();
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
