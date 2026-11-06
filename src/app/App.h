#pragma once
// The sandbox app: window, input, fixed 60 Hz sim loop, brushes, rendering
// and the ImGui panels (sandbox tools + debug overlay).
#include "GpuGasSolver.h"
#include "Renderer.h"
#include "sim/Bodies.h"
#include "sim/Brush.h"
#include "sim/GasGrid.h"
#include "sim/Material.h"
#include "sim/Particles.h"
#include "sim/Player.h"
#include "sim/Reactions.h"
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
    std::string scene = "demo"; // demo | forge | churn | thermal | gas | quarry | play | empty
    int  threads = 0;           // 0 = all hardware threads
    std::uint64_t seed = 2026;
    bool tempView = false;      // start in the temperature view
    bool gasCpu = false;        // run the gas grid on the CPU solver even when the GPU one works
    int  gasView = 0;           // 0 gas, 1 velocity, 2 pressure (F6 cycles)
    bool gasSelfTest = false;   // compare the GPU and CPU gas solvers, print the result, exit
    bool autoplay = false;      // drive the player through a fixed demo (walk, jet, blast, grapple) for smoke runs
};

class App {
public:
    explicit App(AppOptions opts);
    ~App();
    bool init();
    int run();
    int gasSelfTest(); // 0 when the GPU solver matches the CPU reference

private:
    static constexpr int kViewW = 640, kViewH = 360;
    static constexpr int kWorldChunksW = 16, kWorldChunksH = 8; // 1024 x 512 pixels
    static constexpr double kTickDt = 1.0 / 60.0;

    void handleEvent(const SDL_Event& e);
    void updateInput(float dt);
    void stepSim(double frameDt);
    void tickSim();       // one sim tick, then bodies, particles and the gas grid step
    void drawParticles();
    void drawPhase4Ui();
    void drawGas();       // gas over the pixels, or the velocity / pressure view
    void drawGasUi();
    // phase 5: the player
    void spawnPlayer(float x, float y, bool kit);
    void removePlayer();
    void updatePlayerInput(const bool* keys);
    void drawPlayer();
    void drawPlayerUi();
    void blast(int x, int y);
    void loadScene(const std::string& name);
    void drawWorld();
    void drawUi();
    void drawOverlays(const Viewport& vp);
    bool screenToWorld(float sx, float sy, int& wx, int& wy) const;
    bool writeScreenshot(const std::string& path) const;
    void rebuildDisplayPalette();
    void buildHeatPalettes();

    AppOptions opts_;
    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    Renderer renderer_;
    bool imguiReady_ = false;
    bool running_ = true;

    MaterialRegistry mats_;
    std::vector<std::string> materialErrors_;
    std::vector<std::array<std::uint32_t, 256>> displayPalette_; // opaque, blended over air
    std::unique_ptr<ReactionTable> reactions_;
    std::unique_ptr<World> world_;
    std::unique_ptr<ThreadPool> pool_;
    std::unique_ptr<Simulator> sim_;
    std::unique_ptr<GasGrid> gas_;
    std::unique_ptr<ParticleSystem> particles_;
    std::unique_ptr<BodySystem> bodies_;
    bool particlesOn_ = true;
    std::unique_ptr<Player> player_;
    bool playerOn_ = false;     // a player is in the world and takes the keys
    PlayerInput playerIn_;
    bool usePressed_ = false;   // LMB went down since the last tick
    bool creative_ = false;
    bool placePinned_ = true;
    bool slowMo_ = false;
    float savedSpeed_ = 1.0f;
    MaterialId playerMat_ = kEmpty;
    float playerHurt_ = 0.0f;   // recent damage, for the red flash
    int autoFrame_ = 0;
    void autoplay();
    std::unique_ptr<GpuGasSolver> gpuGas_; // null when the GPU path is unavailable
    std::string gpuGasError_;
    bool useGpuGas_ = true;
    int  gasView_ = 0;          // 0 gas, 1 velocity, 2 pressure
    bool showGas_ = true;
    float blastPower_ = 1.5f;
    int  blastRadius_ = 16;
    std::vector<std::uint32_t> gasColor_; // per species, RGBA8

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
    bool showUi_ = true, showChunks_ = false, showDirty_ = false;
    bool showTemp_ = false, glow_ = true; // temperature view (F5), hot pixels glow
    std::array<std::uint32_t, 1024> tempPalette_{}; // -128 .. 1918 C in 2 C steps
    std::array<std::uint32_t, 256> glowColor_{};    // 400 .. 1675 C in 5 C steps, alpha in the top byte
    std::array<float, 120> tickHistory_{};
    int tickHistoryPos_ = 0;
    double frameMs_ = 0.0;
};

} // namespace notita
