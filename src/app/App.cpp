#include "App.h"
#include "Brand.h"

#include "script/ReactionScript.h"
#include "sim/Scenes.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    gpuGas_.reset(); // its GPU buffers must go before the device does (it was released after ~App, on a dead device)
    gpuLight_.reset();
    gpuLiquid_.reset();
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
    particles_ = std::make_unique<ParticleSystem>(*world_, mats_, pool_.get(), opts_.seed);
    bodies_ = std::make_unique<BodySystem>(*world_, mats_, particles_.get(), opts_.seed);
    sim_->setParticles(particles_.get());
    particles_->setGas(gas_.get());
    gas_->setParticles(particles_.get());
    bodies_->setGas(gas_.get());
    player_ = std::make_unique<Player>(*world_, mats_, opts_.seed);
    player_->setGas(gas_.get());
    player_->setBodies(bodies_.get());
    electric_ = std::make_unique<ElectricSystem>(*world_, mats_, pool_.get(), opts_.seed);
    for (const auto& e : electric_->errors()) SDL_Log("electricity: %s", e.c_str());
    materialErrors_.insert(materialErrors_.end(), electric_->errors().begin(), electric_->errors().end());
    player_->setElectric(electric_.get());
    gen_ = std::make_unique<WorldGen>(mats_, opts_.seed);
    showcase_ = std::make_unique<Showcase>(mats_);
    {
        StreamParams sp;
        sp.chunksW = kWorldChunksW;
        sp.chunksH = kWorldChunksH;
        stream_ = std::make_unique<Streamer>(*world_, mats_, *gen_, sp);
        stream_->attach(sim_.get(), gas_.get(), particles_.get(), bodies_.get());
        stream_->setPool(pool_.get());
        stream_->setElectric(electric_.get());
        // a save of the showcase brings the building back (the generator lays it over the terrain)
        stream_->setOverlayHook([this](const std::string& name) {
            if (name == showcase_->name()) gen_->setOverlay(showcase_.get());
            else if (name.empty()) gen_->setOverlay(nullptr);
            else return false;
            return true;
        });
    }
    if (!opts_.load.empty()) savePath_ = opts_.load;
    gasView_ = opts_.gasView;
    for (int k = 0; k < gas_->speciesCount(); ++k) gasColor_.push_back(mats_[gas_->species(k)].color);
    brush_.material = mats_.idOf("sand");
    brush_.radius = 6;
    loadScene(opts_.scene);
    if (opts_.at) spawnPlayer(opts_.atX, opts_.atY, !playerOn_);
    showTemp_ = opts_.tempView;
    showElectric_ = opts_.electricView >= 0;
    electricView_ = std::max(0, opts_.electricView);

    // ---- window + GPU ----
    SDL_SetAppMetadata(brand::kName, brand::kVersion, nullptr);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    window_ = SDL_CreateWindow(brand::kName, kViewW * 2, kViewH * 2,
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
    initLighting();
    initLiquid();

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
    for (std::size_t i = 0; i < mats_.size(); ++i) {
        const Material& m = mats_[static_cast<MaterialId>(i)];
        // `noise = 0.1` in materials.txt: per-channel hue jitter on top of the brightness
        // spread (color_variation), hashed from the shade so a pixel keeps its colour
        float noise = 0.0f;
        if (auto it = m.extra.find("noise"); it != m.extra.end()) noise = std::clamp(std::strtof(it->second.c_str(), nullptr), 0.0f, 0.5f);
        for (int s = 0; s < 256; ++s) {
            std::uint32_t c = m.palette[s];
            if (noise > 0.0f) {
                std::uint32_t h = (static_cast<std::uint32_t>(s) * 0x9E3779B1u) ^ (static_cast<std::uint32_t>(i) * 0x85EBCA77u);
                std::uint32_t out = c & 0xFF000000u;
                for (int k = 0; k < 3; ++k) {
                    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
                    const float j = (static_cast<float>(h & 0xFFFF) / 32767.5f - 1.0f) * noise; // -noise .. +noise
                    const float v = static_cast<float>((c >> (8 * k)) & 0xFF) * (1.0f + j);
                    out |= static_cast<std::uint32_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f) << (8 * k);
                }
                c = out;
            }
            displayPalette_[i][s] = blendOver(c, air);
        }
    }
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

void App::startWorld() {
    float sx = 0.5f, sy = static_cast<float>(gen_->surfaceY(0));
    if (gen_->overlay() == showcase_.get()) { sx = showcase_->spawnX(); sy = showcase_->spawnY(); }
    stream_->start(static_cast<int>(std::floor(sx)), static_cast<int>(sy));
    streaming_ = true;
    spawnPlayer(sx, sy, true);
    player_->setSpawnPoint(sx, sy);
    if (opts_.autoplay) player_->inventory().capacity = 1LL << 40; // the smoke run digs a long tunnel
    worldMsg_.clear();
}

void App::followWorld() {
    if (!streaming_) return;
    const float fx = playerOn_ ? player_->x() : camX_ + kViewW * 0.5f;
    const float fy = playerOn_ ? player_->centreY() : camY_ + kViewH * 0.5f;
    stream_->follow(fx, fy);
}

bool App::saveWorld() {
    if (!streaming_) { worldMsg_ = "save/load works in the streamed scenes (O world, I showcase)"; return false; }
    SaveExtras ex;
    ex.hasPlayer = playerOn_;
    ex.player = player_->state();
    const Inventory& inv = player_->inventory();
    ex.creative = inv.creative;
    ex.capacity = inv.capacity;
    ex.charges = inv.charges;
    for (std::size_t m = 0; m < inv.counts().size(); ++m)
        if (inv.counts()[m]) ex.inventory.emplace_back(static_cast<MaterialId>(m), inv.counts()[m]);
    std::string err;
    const auto t0 = SDL_GetTicksNS();
    const bool ok = stream_->save(savePath_, ex, &err);
    const double ms = static_cast<double>(SDL_GetTicksNS() - t0) / 1e6;
    worldMsg_ = ok ? "saved " + savePath_ + " (" + std::to_string(static_cast<int>(ms)) + " ms)" : "save failed: " + err;
    SDL_Log("%s", worldMsg_.c_str());
    return ok;
}

bool App::loadWorld() {
    SaveExtras ex;
    std::string err;
    if (!stream_->load(savePath_, ex, &err)) {
        worldMsg_ = "load failed: " + err;
        SDL_Log("%s", worldMsg_.c_str());
        return false;
    }
    streaming_ = true;
    showcaseScene_ = gen_->overlay() == showcase_.get();
    if (ex.hasPlayer) {
        player_->setState(ex.player);
        Inventory& inv = player_->inventory();
        inv.reset(mats_.size());
        inv.creative = ex.creative;
        inv.capacity = ex.capacity;
        inv.charges = ex.charges;
        for (const auto& [m, n] : ex.inventory) inv.setCount(m, n);
        creative_ = ex.creative;
        playerOn_ = true;
        camX_ = player_->x() - kViewW * 0.5f;
        camY_ = player_->y() - kViewH * 0.6f;
    } else {
        removePlayer();
        camX_ = static_cast<float>(world_->minX() + (world_->widthPx() - kViewW) / 2);
        camY_ = static_cast<float>(world_->minY() + (world_->heightPx() - kViewH) / 2);
    }
    worldMsg_ = "loaded " + savePath_ + (err.empty() ? "" : " (" + err + ")");
    SDL_Log("%s", worldMsg_.c_str());
    return true;
}

void App::loadScene(const std::string& name) {
    if (name == "world" || name == "showcase") {
        if (particles_) particles_->clear();
        if (bodies_) bodies_->clear();
        if (electric_) electric_->clear();
        gen_->setOverlay(name == "showcase" ? showcase_.get() : nullptr);
        showcaseScene_ = name == "showcase";
        circuitsScene_ = false;
        startWorld();
        if (!opts_.load.empty()) { loadWorld(); opts_.load.clear(); }
        return;
    }
    if (streaming_) {
        stream_->reset(0, 0); // back to the fixed sandbox world
        streaming_ = false;
    }
    showcaseScene_ = false;
    if (particles_) particles_->clear();
    if (bodies_) bodies_->clear();
    if (electric_) electric_->clear();
    removePlayer(); // only the play scene has a player (F7 spawns one anywhere)
    float psx = 0, psy = 0;
    const bool play = name == "play" || name == "playground";
    if (play) scenes::playground(*world_, mats_, opts_.seed, &psx, &psy);
    else if (name == "churn") scenes::churn(*world_, mats_, opts_.seed);
    else if (name == "quarry") scenes::quarry(*world_, mats_, opts_.seed);
    else if (name == "forge") scenes::forge(*world_, mats_, opts_.seed);
    else if (name == "thermal") scenes::thermalChurn(*world_, mats_, opts_.seed);
    else if (name == "gas") scenes::gasLab(*world_, mats_, opts_.seed);
    else if (name == "circuits") scenes::circuits(*world_, mats_, opts_.seed);
    else if (name == "empty") world_->clear();
    else scenes::demo(*world_, mats_, opts_.seed);
    circuitsScene_ = name == "circuits";
    circuitTick_ = 0;
    if (gas_) gas_->clear();
    camX_ = static_cast<float>(world_->minX() + (world_->widthPx() - kViewW) / 2);
    camY_ = static_cast<float>(world_->minY() + world_->heightPx() - kViewH);
    if (play) spawnPlayer(psx, psy, true);
}

void App::spawnPlayer(float x, float y, bool kit) {
    player_->spawn(x, y);
    Inventory& inv = player_->inventory();
    inv.creative = creative_;
    if (kit) {
        // a starting kit for play mode: water for the jet, timber for a raft or two, a few charges
        inv.reset(mats_.size());
        inv.add(mats_.idOf("water"), 240);
        inv.add(mats_.idOf("timber"), 600);
        inv.charges = 6;
        playerMat_ = mats_.idOf("timber");
    }
    if (playerMat_ == kEmpty) playerMat_ = mats_.idOf("sand");
    playerOn_ = true;
    camX_ = x - kViewW * 0.5f;
    camY_ = y - kViewH * 0.6f;
}

void App::removePlayer() { playerOn_ = false; }

void App::blast(int x, int y) {
    blastAt(gas_.get(), bodies_.get(), playerOn_ ? player_.get() : nullptr, x, y, blastRadius_, blastPower_);
}

// --autoplay: a fixed run through the play scene, by frame, with the aim
// relative to the player (walk, jet over the hill, a blast jump, a grapple).
void App::autoplay() {
    const int f = autoFrame_++;
    PlayerInput& in = playerIn_;
    const float px = player_->x(), py = player_->centreY();
    in.move = 0.0f;
    in.jump = in.down = in.use = in.place = false;
    in.aimX = px + 30.0f;
    in.aimY = py;
    if (showcaseScene_) {
        // --scene showcase --autoplay: walk east from the lobby, up the stairs, through the
        // gallery and into the demolition hall, then dig on east (streaming as it goes).
        // With --at the player stays put (screenshots of one room with its switches closed).
        if (opts_.at) return;
        in.move = 1.0f;
        in.jump = f > 200 && f % 60 < 10;
        if (f > 700) { in.tool = Tool::Dig; in.use = true; in.aimX = px + 12.0f; in.aimY = py; }
        return;
    }
    if (streaming_) {
        // --scene world --autoplay: walk east for good, digging through whatever is in the way
        // and hopping now and then, so the awake window keeps shifting (a streaming smoke run)
        in.move = 1.0f;
        in.tool = Tool::Dig;
        in.use = true;
        in.aimX = px + 12.0f;
        in.aimY = py + 3.0f;
        in.jump = f % 50 < 12;
        return;
    }
    if (f < 60) in.move = 1.0f;
    else if (f < 130) { in.tool = Tool::Jet; in.use = true; in.move = 1.0f; in.aimX = px - 10.0f; in.aimY = py + 40.0f; }
    else if (f < 220) in.move = 1.0f;
    else if (f == 220) { in.tool = Tool::Blast; usePressed_ = true; in.aimX = px - 4.0f; in.aimY = player_->y() + 2.0f; }
    else if (f < 300) in.move = 1.0f;
    else if (f == 300) { in.tool = Tool::Grapple; usePressed_ = true; in.aimX = px + 80.0f; in.aimY = py - 140.0f; }
    else if (f < 380) in.jump = true;
    else in.move = 1.0f;
}

void App::updatePlayerInput(const bool* keys) {
    PlayerInput& in = playerIn_;
    const bool kb = !ImGui::GetIO().WantCaptureKeyboard;
    in.move = 0.0f;
    if (kb && (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])) in.move -= 1.0f;
    if (kb && (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT])) in.move += 1.0f;
    in.jump = kb && (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_SPACE]);
    in.down = kb && (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]);
    in.use = painting_;
    in.place = erasing_; // RMB places in play mode
    int wx = 0, wy = 0;
    screenToWorld(mouseX_, mouseY_, wx, wy);
    in.aimX = static_cast<float>(wx) + 0.5f;
    in.aimY = static_cast<float>(wy) + 0.5f;
    in.material = playerMat_;
    in.brushRadius = std::clamp(brush_.radius, 0, 12);
    in.shape = brush_.shape;
    in.placePinned = placePinned_;
    player_->inventory().creative = creative_;
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
        if (e.button.button == SDL_BUTTON_LEFT && playerOn_) usePressed_ = true;
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
        if (playerOn_ && !(SDL_GetModState() & SDL_KMOD_SHIFT)) {
            // cycle the material in hand: everything in creative, what the inventory holds in play
            const Inventory& inv = player_->inventory();
            std::vector<MaterialId> list;
            for (std::size_t i = 1; i < mats_.size(); ++i)
                if (creative_ ? mats_[static_cast<MaterialId>(i)].kind != MaterialKind::Empty : inv.count(static_cast<MaterialId>(i)) > 0)
                    list.push_back(static_cast<MaterialId>(i));
            if (list.empty()) break;
            auto it = std::find(list.begin(), list.end(), playerMat_);
            int idx = it == list.end() ? 0 : static_cast<int>(it - list.begin()) + (e.wheel.y > 0 ? -1 : 1);
            idx = (idx % static_cast<int>(list.size()) + static_cast<int>(list.size())) % static_cast<int>(list.size());
            playerMat_ = list[static_cast<std::size_t>(idx)];
            break;
        }
        brush_.radius = std::clamp(brush_.radius + (e.wheel.y > 0 ? 1 : -1) * std::max(1, brush_.radius / 6), 0, 64);
        break;
    case SDL_EVENT_KEY_DOWN: {
        if (io.WantCaptureKeyboard) break;
        const SDL_Keycode k = e.key.key;
        if (k == SDLK_F7) {
            // spawn the player at the cursor (or take it out)
            int sx, sy;
            if (playerOn_) removePlayer();
            else if (screenToWorld(mouseX_, mouseY_, sx, sy)) spawnPlayer(static_cast<float>(sx) + 0.5f, static_cast<float>(sy), false);
            break;
        }
        if (k == SDLK_L) { loadScene("play"); break; }
        if (k == SDLK_O) { loadScene("world"); break; }
        if (k == SDLK_I) { loadScene("showcase"); break; }
        if (k == SDLK_F8) { saveWorld(); break; }
        if (k == SDLK_F9) { loadWorld(); break; }
        if (k == SDLK_F2) { creative_ = !creative_; break; }
        // electricity: E flips the switch at the cursor (or the one in reach of the player),
        // Y calls lightning down onto the cursor's column, M shows current / potential
        if (k == SDLK_E) {
            int sx, sy;
            if (playerOn_) toggleSwitchAt(static_cast<int>(std::floor(player_->x())), static_cast<int>(std::floor(player_->centreY())));
            else if (screenToWorld(mouseX_, mouseY_, sx, sy)) toggleSwitchAt(sx, sy);
            break;
        }
        if (k == SDLK_Y) {
            int sx, sy;
            if (screenToWorld(mouseX_, mouseY_, sx, sy)) electric_->strike(sx, std::max(world_->minY(), static_cast<int>(camY_)));
            break;
        }
        if (k == SDLK_M) {
            if (showElectric_ && electricView_ == 0) electricView_ = 1;
            else if (showElectric_) { showElectric_ = false; electricView_ = 0; }
            else showElectric_ = true;
            break;
        }
        if (k == SDLK_U) { loadScene("circuits"); break; }
        if (k == SDLK_T) {
            slowMo_ = !slowMo_;
            if (slowMo_) { savedSpeed_ = speed_; speed_ = 0.25f; } else speed_ = savedSpeed_;
            break;
        }
        if (playerOn_) {
            // the player has WASD / Space; pause moves to P, tools to 1-6
            if (k == SDLK_SPACE || k == SDLK_W || k == SDLK_A || k == SDLK_S || k == SDLK_D) break;
            if (k == SDLK_P) { paused_ = !paused_; break; }
            if (k == SDLK_Z) { placePinned_ = !placePinned_; break; }
            if (k >= SDLK_1 && k <= SDLK_6) { playerIn_.tool = static_cast<Tool>(k - SDLK_1); break; }
        }
        if (k == SDLK_SPACE) paused_ = !paused_;
        else if (k == SDLK_PERIOD || k == SDLK_N) { paused_ = true; ++stepRequests_; }
        else if (k == SDLK_TAB || k == SDLK_F1) showUi_ = !showUi_;
        else if (k == SDLK_F3) showChunks_ = !showChunks_;
        else if (k == SDLK_F4 && (SDL_GetModState() & SDL_KMOD_SHIFT)) showDirty_ = !showDirty_;
        else if (k == SDLK_F4) lightOn_ = !lightOn_;
        else if (k == SDLK_F5) showTemp_ = !showTemp_;
        else if (k == SDLK_F10) setNewLookAndMotion(!(newLook_ && smoothPowder_ && calmLiquid_));
        else if (k == SDLK_F6) gasView_ = (gasView_ + 1) % 3;
        else if (k == SDLK_G) gas_->params().enabled = !gas_->params().enabled;
        else if (k == SDLK_V) loadScene("gas");
        else if (k == SDLK_Q) loadScene("quarry");
        else if (k == SDLK_K) {
            int bx, by;
            if (screenToWorld(mouseX_, mouseY_, bx, by)) bodies_->releaseClump(bx, by);
        }
        else if (k == SDLK_X) {
            int bx, by;
            if (screenToWorld(mouseX_, mouseY_, bx, by)) blast(bx, by);
        }
        else if (k == SDLK_H) brush_.mode = BrushMode::Heat;
        else if (k == SDLK_J) brush_.mode = BrushMode::Cool;
        else if (k == SDLK_P) brush_.mode = BrushMode::Place;
        else if (k == SDLK_F) loadScene("forge");
        else if (k == SDLK_LEFTBRACKET) brush_.radius = std::max(0, brush_.radius - 1);
        else if (k == SDLK_RIGHTBRACKET) brush_.radius = std::min(64, brush_.radius + 1);
        else if (k == SDLK_R) loadScene("demo");
        else if (k == SDLK_B) loadScene("churn");
        else if (k == SDLK_C) { particles_->clear(); bodies_->clear(); world_->clear(); gas_->clear(); electric_->clear(); }
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
    if (playerOn_) {
        updatePlayerInput(keys);
        if (opts_.autoplay) autoplay();
        const float tx = player_->x() - kViewW * 0.5f, ty = player_->centreY() - kViewH * 0.55f;
        const float k = std::min(1.0f, dt * 8.0f);
        camX_ += (tx - camX_) * k;
        camY_ += (ty - camY_) * k;
        camX_ = std::clamp(camX_, static_cast<float>(world_->minX()), static_cast<float>(world_->minX() + std::max(0, world_->widthPx() - kViewW)));
        camY_ = std::clamp(camY_, static_cast<float>(world_->minY()), static_cast<float>(world_->minY() + std::max(0, world_->heightPx() - kViewH)));
        return; // no free brush painting while playing (creative mode places with RMB, F7 leaves the player)
    }
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
        if (opts_.lockstep) accumulator_ = kTickDt; // smoke runs / screenshots: frame N is tick N
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
    if (opts_.autoplay && circuitsScene_) autoCircuits();
    if (opts_.autoplay && showcaseScene_) autoShowcase();
    sim_->setParticles(particlesOn_ ? particles_.get() : nullptr);
    sim_->params().smoothPowder = smoothPowder_; // #477 old/new toggle (F10)
    sim_->params().calmLiquid = calmLiquid_;
    sim_->tick();
    electric_->setPlayer(playerOn_ ? player_.get() : nullptr);
    electric_->step();
    sim_->params().ambientRate = gas_->params().enabled ? 0.0f : sim_->params().ambientRate;
    bodies_->step();
    particles_->step();
    gas_->step();
    if (playerOn_) {
        playerIn_.usePressed = usePressed_;
        usePressed_ = false;
        player_->step(playerIn_);
        playerIn_.usePressed = false;
        playerHurt_ = std::max(playerHurt_ * 0.9f, player_->stats().damage);
    }
    followWorld();
}

void App::drawPlayer() {
    if (!playerOn_) return;
    auto& px = renderer_.pixels();
    const int cx0 = static_cast<int>(camX_), cy0 = static_cast<int>(camY_);
    auto put = [&](int wx, int wy, std::uint32_t col) {
        const int x = wx - cx0, y = wy - cy0;
        if (x >= 0 && y >= 0 && x < kViewW && y < kViewH) {
            px[static_cast<std::size_t>(y) * kViewW + x] = col;
            clearLiquidInfo(x, y);
        }
    };
    // the rope first, so the player is drawn over it
    if (player_->roped()) {
        int x0 = static_cast<int>(std::floor(player_->x())), y0 = static_cast<int>(std::floor(player_->centreY()));
        const int x1 = static_cast<int>(std::floor(player_->ropeX())), y1 = static_cast<int>(std::floor(player_->ropeY()));
        const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (int guard = 0; guard < 2000; ++guard) {
            put(x0, y0, 0xFF6EAAC8u);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
    const PlayerParams& pp = player_->params();
    const int ix = static_cast<int>(std::floor(player_->x())), iy = static_cast<int>(std::floor(player_->y()));
    if (player_->stats().jetMass > 0) {
        // the jet's gas is real (it's in the grid, see the velocity view), but one unit a tick is a
        // faint tint, so draw a short flickering plume at the nozzle as well
        float dx = playerIn_.aimX - player_->x(), dy = playerIn_.aimY - player_->centreY();
        const float d = std::max(0.5f, std::sqrt(dx * dx + dy * dy));
        dx /= d;
        dy /= d;
        Rng g(Rng::hash(static_cast<std::uint64_t>(sim_->tickCount()), 77));
        const float out = std::abs(dy) * (pp.height * 0.5f + 1.5f) + std::abs(dx) * (pp.halfWidth + 2.0f);
        for (int k = 0; k < 14; ++k) {
            const float t = out + g.unit() * 9.0f, side = (g.unit() - 0.5f) * (1.0f + t * 0.3f);
            const int wx = static_cast<int>(std::floor(player_->x() + dx * t - dy * side));
            const int wy = static_cast<int>(std::floor(player_->centreY() + dy * t + dx * side));
            put(wx, wy, t < out + 3.0f ? 0xFF8CE6FFu : 0xFFDCE0E4u);
        }
    }
    const int face = player_->facing();
    const bool walking = std::abs(player_->vx()) > 0.2f && player_->grounded();
    const int stride = (ix / 3) & 1;
    const std::uint32_t hurtW = static_cast<std::uint32_t>(std::clamp(playerHurt_ * 120.0f, 0.0f, 180.0f));
    for (int r = 0; r < pp.height; ++r) {
        const int wy = iy - pp.height + r;
        for (int dx = -pp.halfWidth; dx <= pp.halfWidth; ++dx) {
            if (!player_->covers(ix + dx, wy)) continue;
            std::uint32_t col;
            const int fx = dx * face; // + towards where it looks
            if (r <= 1) col = 0xFF5A5F1Eu;                                   // hood
            else if (r <= 5) col = (fx >= -1 && fx <= 1) ? 0xFFA0C9E8u : 0xFF5A5F1Eu; // face in the hood
            else if (r == 11) col = 0xFF3CAADCu;                             // belt
            else if (r <= 16) col = (fx == pp.halfWidth) ? 0xFF6A7A26u : 0xFF838F2Fu; // cloak, lit edge
            else {
                // legs with a gap between them; while walking the lower rows step one leg at a time
                const bool show = dx != 0 && (!walking || r < 18 || (stride ? dx < 0 : dx > 0));
                if (!show) continue;
                col = 0xFF1E2D46u;
            }
            if (r == 3 && fx == 1) col = 0xFF1E1414u; // the eye
            if (player_->gliding() && r >= 6 && r <= 12 && std::abs(dx) == pp.halfWidth) {
                // the cloak spread out to the sides
                for (int k = 1; k <= 3 && r - 6 >= k - 1; ++k) put(ix + dx + (dx > 0 ? k : -k), wy + k - 1, 0xFF6A7A26u);
            }
            if (hurtW > 0) col = mixColor(col, 0xFF2020E0u, hurtW);
            put(ix + dx, wy, col);
        }
    }
}

void App::drawParticles() {
    auto& px = renderer_.pixels();
    const int cx0 = static_cast<int>(camX_), cy0 = static_cast<int>(camY_);
    for (const Particle& p : particles_->particles()) {
        const int x = static_cast<int>(std::floor(p.x)) - cx0, y = static_cast<int>(std::floor(p.y)) - cy0;
        if (x < 0 || y < 0 || x >= kViewW || y >= kViewH) continue;
        px[static_cast<std::size_t>(y) * kViewW + x] = displayPalette_[p.cell.material][p.cell.shade];
        markLiquidParticle(x, y, p.cell.material); // #477: spray joins the liquid surface as foam
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
                if (showTemp_) {
                    // temperature view: the heat colour, with a little of the material showing through
                    for (int x = x0; x < x1; ++x) {
                        const Cell& c = cells[x - ox];
                        const int ti = std::clamp(static_cast<int>((c.temperature + 128.0f) * 0.5f), 0, 1023);
                        const std::uint32_t base = displayPalette_[c.material][c.shade];
                        row[x - cx0] = mixColor(base, tempPalette_[static_cast<std::size_t>(ti)], c.material == kEmpty ? 235 : 190);
                    }
                } else if (glow_ && !lightingActive()) { // with lighting on, heat glows as emitted light instead
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
    shadeLiquids();
    drawParticles();
    drawGas();
    drawPlayer();
    drawElectric();
    prepareLight();
}

void App::autoCircuits() {
    const int t = circuitTick_++;
    if (t == 30) {
        // close every switch in the scene (each call flips one connected switch)
        const MaterialId open = mats_.idOf("switch_open");
        for (int y = world_->minY(); y < world_->minY() + world_->heightPx(); ++y)
            for (int x = world_->minX(); x < world_->minX() + world_->widthPx(); ++x)
                if (world_->material(x, y) == open && open != kEmpty) electric_->toggleSwitch(x, y, 0);
    }
    if (t == 240) {
        const int vx = world_->minX() + std::max(0, (world_->widthPx() - kViewW) / 2);
        electric_->strike(vx + 600, world_->minY());
        electric_->strike(vx + 561, world_->minY());
    }
    if (t % 120 == 0 && t > 0) {
        const ElectricStats& es = electric_->stats();
        SDL_Log("circuits t=%d: %d live px, %d circuits (%d settling), %d it, power %.1f, %d lamp px lit, sparks %d, splits %lld, max %.0f V, %.2f ms",
                t, es.nodes, es.components, es.unsettled, es.iterations, es.power, es.lit, es.sparks, es.splitsTotal,
                static_cast<double>(electric_->maxPotential()), es.ms);
    }
}

// --scene showcase --autoplay: every switch in the awake window closes at t=60
// (furnace igniter, gas chamber fuse, heater, electrolysis, the shelf lamp);
// stats every 120 ticks.
void App::autoShowcase() {
    const int t = circuitTick_++;
    if (t == 60) {
        const MaterialId open = mats_.idOf("switch_open");
        for (int y = world_->minY(); y < world_->minY() + world_->heightPx(); ++y)
            for (int x = world_->minX(); x < world_->minX() + world_->widthPx(); ++x)
                if (world_->material(x, y) == open && open != kEmpty) electric_->toggleSwitch(x, y, 0);
    }
    if (t % 120 == 0 && t > 0) {
        const ElectricStats& es = electric_->stats();
        const BodyStats& bs = bodies_->stats();
        SDL_Log("showcase t=%d: player (%.0f, %.0f), %d lamp px lit, %d circuits, power %.1f, sparks %d, splits %lld, bodies %d, "
                "shifts %d, %zu asleep, electric %.2f ms",
                t, static_cast<double>(player_->x()), static_cast<double>(player_->y()), es.lit, es.components, es.power, es.sparks,
                es.splitsTotal, bs.bodies, stream_->stats().shifts, stream_->storedChunks(), es.ms);
    }
}

void App::toggleSwitchAt(int x, int y) {
    const int reach = playerOn_ ? static_cast<int>(player_->params().reach) : 12;
    const int n = electric_->toggleSwitch(x, y, reach);
    if (n > 0) SDL_Log("switch: %d px flipped", n);
}

void App::drawElectric() {
    if (!showElectric_) return;
    auto& px = renderer_.pixels();
    const int cx0 = static_cast<int>(camX_), cy0 = static_cast<int>(camY_);
    const float vmax = std::max(1.0f, electric_->maxPotential());
    // darken everything so the wires stand out, then paint the field over the conductors
    for (auto& p : px) p = mixColor(p, 0xFF000000u, 150);
    for (int ccy = cy0 >> kChunkShift; ccy <= (cy0 + kViewH - 1) >> kChunkShift; ++ccy)
        for (int ccx = cx0 >> kChunkShift; ccx <= (cx0 + kViewW - 1) >> kChunkShift; ++ccx) {
            const float* v = electric_->potentials(ccx, ccy);
            const float* cur = electric_->currents(ccx, ccy);
            const Chunk* ch = world_->chunk(ccx, ccy);
            if (!v || !cur || !ch) continue;
            const int ox = ccx * kChunkSize, oy = ccy * kChunkSize;
            const int x0 = std::max(ox, cx0), x1 = std::min(ox + kChunkSize, cx0 + kViewW);
            const int y0 = std::max(oy, cy0), y1 = std::min(oy + kChunkSize, cy0 + kViewH);
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    const std::size_t l = static_cast<std::size_t>(((y - oy) << kChunkShift) | (x - ox));
                    if (!electric_->conductive(ch->cells[l].material) || std::isnan(v[l])) continue;
                    std::uint32_t col;
                    if (electricView_ == 0) {
                        // current: dim blue (none) .. cyan .. white, log scale
                        const float t = std::clamp(std::log10(1.0f + cur[l] * 100.0f) / 3.0f, 0.0f, 1.0f);
                        const auto r = static_cast<std::uint32_t>(40.0f + 215.0f * t * t);
                        const auto g = static_cast<std::uint32_t>(60.0f + 195.0f * t);
                        const auto b = static_cast<std::uint32_t>(120.0f + 135.0f * std::sqrt(t));
                        col = 0xFF000000u | (b << 16) | (g << 8) | r;
                    } else {
                        // potential: blue (0 V) .. magenta .. yellow (the highest)
                        const float t = std::clamp(v[l] / vmax, -1.0f, 1.0f);
                        const float a = std::abs(t);
                        const auto r = static_cast<std::uint32_t>(60.0f + 195.0f * a);
                        const auto g = static_cast<std::uint32_t>(t >= 0 ? 40.0f + 200.0f * a * a : 40.0f);
                        const auto b = static_cast<std::uint32_t>(t >= 0 ? 200.0f * (1.0f - a) + 30.0f : 230.0f);
                        col = 0xFF000000u | (std::min(b, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(r, 255u);
                    }
                    px[static_cast<std::size_t>(y - cy0) * kViewW + (x - cx0)] = col;
                }
        }
}

void App::drawElectricUi() {
    ImGui::Begin("Electricity", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    const ElectricStats& es = electric_->stats();
    ElectricParams& ep = electric_->params();
    ImGui::Checkbox("On", &ep.enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Overlay (M)", &showElectric_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    ImGui::Combo("##ev", &electricView_, "Current\0Potential\0");
    ImGui::Text("%d live px in %d circuits (%d solved, %d settling), %d CG iterations", es.nodes, es.components, es.solved,
                es.unsettled, es.iterations);
    ImGui::Text("power %.1f, heat %.1f, %d lamp px lit, max %.0f V, %.2f ms", es.power, es.heat, es.lit,
                static_cast<double>(electric_->maxPotential()), es.ms);
    ImGui::Text("sparks %d, splits %d (total %lld)", es.sparks, es.splits, es.splitsTotal);
    if (playerOn_ && player_->stats().shock > 0.0f) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "shock! %.2f", player_->stats().shock);
    ImGui::PushItemWidth(ImGui::GetFontSize() * 11.0f);
    ImGui::SliderFloat("Heat gain", &ep.heatGain, 0.0f, 2.0f, "%.2f");
    ImGui::SliderInt("CG iterations / tick", &ep.maxIterations, 1, 512);
    ImGui::SliderFloat("Spark power", &ep.sparkPower, 0.2f, 20.0f, "%.1f");
    ImGui::SliderFloat("Electrolysis rate", &ep.splitRate, 0.0f, 0.002f, "%.5f");
    ImGui::PopItemWidth();
    ImGui::TextDisabled("E flip switch (cursor / in reach), Y lightning at the cursor's column");
    ImGui::TextDisabled("U circuits scene: volt cell -> wire -> lamp / heater -> ground rod");
    ImGui::End();
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
    // brush outline (with a player: where its tool reaches, clamped to its reach)
    int wx, wy;
    if (playerOn_ && screenToWorld(mouseX_, mouseY_, wx, wy)) {
        const float pcx = player_->x(), pcy = player_->centreY();
        float dx = static_cast<float>(wx) + 0.5f - pcx, dy = static_cast<float>(wy) + 0.5f - pcy;
        const float d = std::sqrt(dx * dx + dy * dy), reach = player_->params().reach;
        if (d > reach) { dx *= reach / d; dy *= reach / d; }
        wx = static_cast<int>(std::floor(pcx + dx));
        wy = static_cast<int>(std::floor(pcy + dy));
        dl->AddCircle(toScreen(pcx, pcy), reach * vp.scale * inv, IM_COL32(255, 255, 255, 25), 48);
        const float r = static_cast<float>(std::clamp(brush_.radius, 0, 12)) + 0.5f;
        dl->AddCircle(toScreen(static_cast<float>(wx) + 0.5f, static_cast<float>(wy) + 0.5f), r * vp.scale * inv, IM_COL32(255, 230, 160, 160), 24);
    } else if (screenToWorld(mouseX_, mouseY_, wx, wy) && !ImGui::GetIO().WantCaptureMouse) {
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
    // "###Sandbox" keeps the window's ImGui id (and its saved place in imgui.ini)
    char sandboxTitle[64];
    std::snprintf(sandboxTitle, sizeof sandboxTitle, "%s: sandbox###Sandbox", brand::kName);
    ImGui::Begin(sandboxTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
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
    if (ImGui::Button("Quarry (Q)")) loadScene("quarry");
    ImGui::SameLine();
    if (ImGui::Button("Play (L)")) loadScene("play");
    ImGui::SameLine();
    if (ImGui::Button("World (O)")) loadScene("world");
    ImGui::SameLine();
    if (ImGui::Button("Showcase (I)")) loadScene("showcase");
    ImGui::SameLine();
    if (ImGui::Button("Circuits (U)")) loadScene("circuits");
    ImGui::SameLine();
    if (ImGui::Button("Clear (C)")) { particles_->clear(); bodies_->clear(); world_->clear(); gas_->clear(); electric_->clear(); }
    ImGui::PopItemWidth();
    ImGui::End();

    drawGasUi();
    drawLightUi();
    drawLiquidUi();
    drawPhase4Ui();
    drawPlayerUi();
    drawWorldUi();
    drawElectricUi();

    // debug overlay
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10, 10), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("Debug", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextDisabled("%s %s, debug", brand::kName, brand::kVersion);
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
    ImGui::Text("particles %d  bodies %d (%d px)  p4 %.2f ms", static_cast<int>(particles_->count()), bodies_->stats().bodies,
                bodies_->stats().pixels, particles_->stats().stepMs + bodies_->stats().stepMs);
    ImGui::Text("tick #%llu  threads %d  seed %llu", static_cast<unsigned long long>(st.tick), pool_->threadCount(),
                static_cast<unsigned long long>(sim_->seed()));
    ImGui::Text("%s", paused_ ? "PAUSED" : (slowMo_ ? "slow motion (T)" : speed_ != 1.0f ? "speed changed" : "60 Hz fixed tick"));
    if (playerOn_)
        ImGui::Text("player (%.1f, %.1f) v (%.2f, %.2f) %s", player_->x(), player_->y(), player_->vx(), player_->vy(),
                    player_->grounded() ? "grounded" : "airborne");
    ImGui::Checkbox("Chunks (F3)", &showChunks_);
    ImGui::SameLine();
    ImGui::Checkbox("Dirty rects (Shift+F4)", &showDirty_);
    int wx, wy;
    if (screenToWorld(mouseX_, mouseY_, wx, wy)) {
        if (const Cell* c = world_->cell(wx, wy))
            ImGui::Text("(%d, %d) %s%s%s%s  %.1f C  vy %.2f", wx, wy, mats_[c->material].label.c_str(),
                        (c->flags & kFlagBurning) ? " (burning)" : "", (c->flags & kFlagBody) ? " (body)" : "",
                        c->aux == kAuxPinned ? " (pinned)" : c->aux == kAuxSettled ? " (settled)" : "", c->temperature, c->vy / 256.0);
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

void App::drawPhase4Ui() {
    SimParams& sp = sim_->params();
    ParticleParams& pp = particles_->params();
    BodyParams& bp = bodies_->params();
    const ParticleStats& ps = particles_->stats();
    const BodyStats& bs = bodies_->stats();
    ImGui::SetNextWindowPos(ImVec2(400, 640), ImGuiCond_FirstUseEver);
    ImGui::Begin("Splashes & bodies", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
    ImGui::Checkbox("Splash particles", &particlesOn_);
    ImGui::SameLine();
    ImGui::Checkbox("Rigid bodies", &bp.enabled);
    ImGui::SliderFloat("Splash speed", &sp.splashSpeed, 2.0f, 12.0f, "%.1f px/tick");
    ImGui::SliderFloat("Restitution", &sp.restitution, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Mix rate", &sp.mixRate, 0.0f, 0.5f, "%.2f");
    ImGui::SliderFloat("Wind on particles", &pp.gasDrag, 0.0f, 0.5f, "%.2f");
    ImGui::SliderFloat("Shatter speed", &bp.shatterSpeed, 1.0f, 20.0f, "%.1f px/tick");
    ImGui::SliderInt("Min body px", &bp.minPixels, 1, 64);
    ImGui::SliderFloat("Wind on bodies", &bp.gasDrag, 0.0f, 0.2f, "%.3f");
    ImGui::SliderFloat("Pressure on bodies", &bp.gasPush, 0.0f, 4.0f, "%.2f");
    ImGui::TextDisabled("K: cut the clump under the cursor loose, X: blast");
    ImGui::Text("particles %d (+%d, settled %d, waiting %d) %.2f ms", ps.count, ps.spawned, ps.settled, ps.waiting, ps.stepMs);
    ImGui::Text("splashed %d px, momentum out/in %.2f", sim_->stats().splashed,
                sim_->stats().splashMomentumIn > 0 ? sim_->stats().splashMomentumOut / sim_->stats().splashMomentumIn : 0.0);
    ImGui::Text("bodies %d (%d awake, %d px, %d hidden) %.2f ms", bs.bodies, bs.awake, bs.pixels, bs.hidden, bs.stepMs);
    ImGui::Text("  search %.2f ms, box2d %.2f ms, %d terrain boxes", bs.findMs, bs.solveMs, bs.terrainShapes);
    ImGui::Text("  this tick: +%d, frozen %d, shattered %d, shoved %d px", bs.created, bs.frozen, bs.shattered, bs.displaced);
    ImGui::PopItemWidth();
    ImGui::End();
}

void App::drawPlayerUi() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10, io.DisplaySize.y - 10), ImGuiCond_FirstUseEver, ImVec2(1, 1));
    ImGui::Begin("Player", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (!playerOn_) {
        ImGui::TextDisabled("No player. L: play scene, F7: spawn at the cursor");
        ImGui::Checkbox("Creative (F2)", &creative_);
        ImGui::End();
        return;
    }
    const PlayerStats& ps = player_->stats();
    const PlayerParams& pp = player_->params();
    Inventory& inv = player_->inventory();
    const float hp = std::max(0.0f, player_->health()) / pp.maxHealth;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hp > 0.5f ? ImVec4(0.3f, 0.8f, 0.4f, 1) : hp > 0.25f ? ImVec4(0.9f, 0.7f, 0.2f, 1) : ImVec4(0.9f, 0.25f, 0.2f, 1));
    char hpText[32];
    std::snprintf(hpText, sizeof hpText, "%.0f hp", player_->health());
    ImGui::ProgressBar(hp, ImVec2(ImGui::GetFontSize() * 14.0f, 0), hpText);
    ImGui::PopStyleColor();
    ImGui::Checkbox("Creative (F2)", &creative_);
    ImGui::SameLine();
    ImGui::Checkbox("Build fixed (Z)", &placePinned_);
    ImGui::SeparatorText("Tool (1-6, LMB)");
    for (int t = 0; t < static_cast<int>(Tool::Count); ++t) {
        char label[48];
        std::snprintf(label, sizeof label, "%d %s", t + 1, toolName(static_cast<Tool>(t)));
        if (t > 0 && t != 3) ImGui::SameLine();
        if (ImGui::RadioButton(label, playerIn_.tool == static_cast<Tool>(t))) playerIn_.tool = static_cast<Tool>(t);
    }
    switch (playerIn_.tool) {
    case Tool::Dig: ImGui::TextDisabled("dig under the brush into the inventory"); break;
    case Tool::Jet: {
        const MaterialId g = player_->jetGas(playerMat_);
        ImGui::TextDisabled("boils or burns the material in hand (or any that works) into gas");
        ImGui::TextDisabled("thrust away from the cursor; %s", g ? ("in hand makes " + mats_[g].label).c_str() : "in hand doesn't work");
        break;
    }
    case Tool::Grapple: ImGui::TextDisabled("click: rope to the cursor / let go; W reel in, S let out"); break;
    case Tool::Blast: ImGui::TextDisabled("click: set off a charge (aim at your feet to jump)"); break;
    case Tool::Raft: ImGui::TextDisabled("click: a %dx%d raft of the solid in hand (timber floats)", pp.raftW, pp.raftH); break;
    case Tool::Freeze: ImGui::TextDisabled("hold: freeze water to walk on"); break;
    default: break;
    }
    ImGui::SeparatorText("In hand (wheel, RMB places)");
    if (playerMat_ != kEmpty) {
        ImGui::ColorButton("##hand", ImGui::ColorConvertU32ToFloat4(toImColor(displayPalette_[playerMat_][160])), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
        ImGui::SameLine();
        if (creative_) ImGui::Text("%s (unlimited)", mats_[playerMat_].label.c_str());
        else ImGui::Text("%s x %lld", mats_[playerMat_].label.c_str(), inv.count(playerMat_));
    }
    if (creative_) {
        ImGui::TextDisabled("creative: every material, charges unlimited");
    } else {
        ImGui::Text("carrying %lld / %lld px, %d charges", inv.total(), inv.capacity, inv.charges);
        ImGui::BeginChild("##inv", ImVec2(ImGui::GetFontSize() * 14.0f, ImGui::GetFontSize() * 7.0f));
        for (std::size_t i = 1; i < mats_.size(); ++i) {
            const long long n = inv.count(static_cast<MaterialId>(i));
            if (n <= 0) continue;
            ImGui::PushID(static_cast<int>(i));
            ImGui::ColorButton("##c", ImGui::ColorConvertU32ToFloat4(toImColor(displayPalette_[i][160])), ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
            ImGui::SameLine();
            char label[96];
            std::snprintf(label, sizeof label, "%s  %lld", mats_[static_cast<MaterialId>(i)].label.c_str(), n);
            if (ImGui::Selectable(label, playerMat_ == i)) playerMat_ = static_cast<MaterialId>(i);
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    if (creative_) {
        // the full palette, like the sandbox panel
        ImGui::BeginChild("##all", ImVec2(ImGui::GetFontSize() * 14.0f, ImGui::GetFontSize() * 7.0f));
        for (std::size_t i = 1; i < mats_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(mats_[static_cast<MaterialId>(i)].label.c_str(), playerMat_ == i)) playerMat_ = static_cast<MaterialId>(i);
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::SeparatorText("Feel");
    ImGui::Text("%s, in liquid %.0f%%, wind (%.1f, %.1f) px/tick", player_->grounded() ? "on the ground" : "in the air", ps.submerged * 100.0f,
                ps.windX, ps.windY);
    ImGui::Text("hottest nearby %.0f C, jet gas %lld units, deaths %d", ps.hottest, ps.jetMassTotal, ps.deaths);
    if (player_->roped()) ImGui::Text("rope %.0f px%s", player_->ropeLength(), player_->ropeBody() ? " (to a body)" : "");
    ImGui::TextDisabled("A/D walk, W/Space jump & swim (hold in a fall to glide), S dive");
    ImGui::TextDisabled("P pause, T slow-mo, F7 leave");
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

void App::drawTitleCard() {
    constexpr double kHold = 2.5, kFade = 1.0; // seconds
    const double t = ImGui::GetTime();
    if (t >= kHold + kFade) return;
    const float a = t < kHold ? 1.0f : static_cast<float>(1.0 - (t - kHold) / kFade);
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float big = ImGui::GetFontSize() * 4.0f, small = ImGui::GetFontSize() * 1.3f;
    const ImVec2 ts = font->CalcTextSizeA(big, FLT_MAX, 0.0f, brand::kName);
    const ImVec2 ps = font->CalcTextSizeA(small, FLT_MAX, 0.0f, brand::kPitch);
    const float cx = io.DisplaySize.x * 0.5f, cy = io.DisplaySize.y * 0.42f;
    const float gap = small * 0.4f, pad = small * 1.2f;
    const float top = cy - (ts.y + gap + ps.y) * 0.5f;
    auto alpha = [&](float v) { return static_cast<int>(v * a); };
    dl->AddRectFilled(ImVec2(0, top - pad), ImVec2(io.DisplaySize.x, top + ts.y + gap + ps.y + pad), IM_COL32(12, 10, 8, alpha(150)));
    const ImVec2 tp(cx - ts.x * 0.5f, top), pp(cx - ps.x * 0.5f, top + ts.y + gap);
    const float sh = std::max(2.0f, big * 0.04f);
    dl->AddText(font, big, ImVec2(tp.x + sh, tp.y + sh), IM_COL32(40, 28, 16, alpha(220)), brand::kName);
    dl->AddText(font, big, tp, IM_COL32(238, 214, 158, alpha(255)), brand::kName);
    dl->AddText(font, small, pp, IM_COL32(220, 210, 190, alpha(230)), brand::kPitch);
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
        drawTitleCard();
        ImGui::Render();
        ImDrawData* dd = ImGui::GetDrawData();

        if (swap) {
            const bool lit = recordLight(cmd); // the GPU lighting writes the view texture itself
            liquidDrawn_ = false;
            if (!recordLiquid(cmd, swap, lit)) renderer_.record(cmd, swap, sw, sh, viewport_, !lit);
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
            // #477: with the liquid surface pass on, the shot is its full-resolution output
            const bool hires = !opts_.screenshot.empty() && liquidScreenshot(opts_.screenshot);
            if (!opts_.screenshot.empty() && !hires) lightScreenshot();
            if (!opts_.screenshot.empty() && !hires && !writeScreenshot(opts_.screenshot))
                SDL_Log("could not write %s", opts_.screenshot.c_str());
            SDL_Log("ran %d frames, last tick %.3f ms, %d/%d chunks active", frame, sim_->stats().tickMs,
                    sim_->stats().activeChunks, sim_->stats().totalChunks);
            if (streaming_)
                SDL_Log("world: player at %.0f,%.0f, %d shifts (last %.2f ms), %d generated, %d woken, %zu chunks asleep (%.0f KiB)",
                        player_->x(), player_->y(), stream_->stats().shifts, stream_->stats().lastShiftMs, stream_->stats().generated,
                        stream_->stats().woken, stream_->storedChunks(), static_cast<double>(stream_->stats().storeBytes) / 1024.0);
            running_ = false;
        }
    }
    return 0;
}

void App::drawWorldUi() {
    ImGui::Begin("World", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (!streaming_) {
        ImGui::TextDisabled("Fixed sandbox world: the streamed procedural world is O");
        if (ImGui::Button("Open the world (O)")) loadScene("world");
        ImGui::SameLine();
        if (ImGui::Button("Load save (F9)")) loadWorld();
        ImGui::End();
        return;
    }
    const StreamStats& st = stream_->stats();
    const int fx = static_cast<int>(std::floor(playerOn_ ? player_->x() : camX_ + kViewW * 0.5f));
    if (gen_->overlay() == showcase_.get()) {
        const int fy = static_cast<int>(std::floor(playerOn_ ? player_->centreY() : camY_ + kViewH * 0.5f));
        const char* here = "outside";
        for (const Showcase::Room& r : showcase_->rooms())
            if (fx >= r.x0 && fx <= r.x1 && fy >= r.y0 && fy <= r.y1) { here = r.name.c_str(); break; }
        ImGui::Text("Showcase: %s   (I restarts it, F8/F9 save/load)", here);
    }
    ImGui::Text("Seed %llu   biome %s   surface y %d", static_cast<unsigned long long>(gen_->seed()), biomeName(gen_->biome(fx)),
                gen_->surfaceY(fx));
    ImGui::Text("Awake window: chunks (%d, %d) .. (%d, %d)", stream_->windowX(), stream_->windowY(),
                stream_->windowX() + stream_->params().chunksW - 1, stream_->windowY() + stream_->params().chunksH - 1);
    ImGui::Text("Sleeping: %zu chunks, %.1f KiB encoded", stream_->storedChunks(), static_cast<double>(st.storeBytes) / 1024.0);
    ImGui::Text("Shifts %d (last %.2f ms)  generated %d  woken %d  slept %d", st.shifts, st.lastShiftMs, st.generated, st.woken, st.slept);
    const long long live = world_->totalMass(mats_) + particles_->totalMass() + bodies_->totalMass() +
                           (playerOn_ ? player_->inventory().totalMass(mats_) : 0);
    ImGui::Text("Mass: live %lld + sleeping %lld px, gas %.1f + %.1f", live, stream_->storedMass(),
                static_cast<double>(gas_->massFixed()) / kGasMassOne, static_cast<double>(stream_->storedGasFixed()) / kGasMassOne);
    if (ImGui::Button("Save (F8)")) saveWorld();
    ImGui::SameLine();
    if (ImGui::Button("Load (F9)")) loadWorld();
    ImGui::SameLine();
    if (ImGui::Button("New world")) startWorld();
    ImGui::TextDisabled("%s save: %s", brand::kName, savePath_.c_str());
    if (!worldMsg_.empty()) ImGui::TextWrapped("%s", worldMsg_.c_str());
    ImGui::End();
}

} // namespace notita
