#pragma once
// The sandbox app: window, input, fixed 60 Hz sim loop, brushes, rendering
// and the ImGui panels (sandbox tools + debug overlay).
#include "Renderer.h"
#include "sim/Brush.h"
#include "sim/Material.h"
#include "sim/Simulator.h"
#include "sim/ThreadPool.h"
#include "sim/World.h"

#include <SDL3/SDL.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace notita {

struct AppOptions {
    int  frames = 0;            // > 0: quit after this many frames (smoke runs)
    std::string screenshot;     // with frames: write the last view to this .bmp
    std::string scene = "demo"; // demo | churn | empty
    int  threads = 0;           // 0 = all hardware threads
    std::uint64_t seed = 2026;
};

class App {
public:
    explicit App(AppOptions opts);
    ~App();
    bool init();
    int run();

private:
    static constexpr int kViewW = 640, kViewH = 360;
    static constexpr int kWorldChunksW = 16, kWorldChunksH = 8; // 1024 x 512 pixels
    static constexpr double kTickDt = 1.0 / 60.0;

    void handleEvent(const SDL_Event& e);
    void updateInput(float dt);
    void stepSim(double frameDt);
    void loadScene(const std::string& name);
    void drawWorld();
    void drawUi();
    void drawOverlays(const Viewport& vp);
    bool screenToWorld(float sx, float sy, int& wx, int& wy) const;
    bool writeScreenshot(const std::string& path) const;
    void rebuildDisplayPalette();

    AppOptions opts_;
    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    Renderer renderer_;
    bool imguiReady_ = false;
    bool running_ = true;

    MaterialRegistry mats_;
    std::vector<std::string> materialErrors_;
    std::vector<std::array<std::uint32_t, 256>> displayPalette_; // opaque, blended over air
    std::unique_ptr<World> world_;
    std::unique_ptr<ThreadPool> pool_;
    std::unique_ptr<Simulator> sim_;

    // sim loop
    bool paused_ = false;
    int  stepRequests_ = 0;
    float speed_ = 1.0f;
    double accumulator_ = 0.0;
    int  ticksThisFrame_ = 0;

    // camera (top-left of the view, world pixels)
    float camX_ = 0, camY_ = 0;

    // brush / input
    Brush brush_;
    Rng brushRng_{1};
    bool painting_ = false, erasing_ = false, panning_ = false;
    int lastWX_ = 0, lastWY_ = 0;
    float mouseX_ = 0, mouseY_ = 0; // window pixels (swapchain space)
    Viewport viewport_;

    // UI / debug
    bool showUi_ = true, showChunks_ = false, showDirty_ = true;
    std::array<float, 120> tickHistory_{};
    int tickHistoryPos_ = 0;
    double frameMs_ = 0.0;
};

} // namespace notita
