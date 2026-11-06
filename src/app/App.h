#pragma once
// The sandbox app: window, input, fixed 60 Hz sim loop, brushes, rendering
// and the ImGui panels (sandbox tools + debug overlay).
#include "GpuFlame.h"
#include "GpuGasSolver.h"
#include "GpuLight.h"
#include "GpuLiquid.h"
#include "Renderer.h"
#include "light/Lighting.h"
#include "sim/Bodies.h"
#include "sim/Brush.h"
#include "sim/Electric.h"
#include "sim/GasGrid.h"
#include "sim/Material.h"
#include "sim/Particles.h"
#include "sim/Player.h"
#include "sim/Reactions.h"
#include "sim/Showcase.h"
#include "sim/Simulator.h"
#include "sim/Streamer.h"
#include "sim/WorldGen.h"
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
    std::string scene = "showcase"; // showcase | world | demo | forge | churn | thermal | gas | quarry | play | circuits | empty
    int  threads = 0;           // 0 = all hardware threads
    std::uint64_t seed = 2026;
    bool tempView = false;      // start in the temperature view
    int  electricView = -1;     // start in the electricity overlay: 0 current, 1 potential
    bool gasCpu = false;        // run the gas grid on the CPU solver even when the GPU one works
    int  gasView = 0;           // 0 gas, 1 velocity, 2 pressure (F6 cycles)
    bool gasSelfTest = false;   // compare the GPU and CPU gas solvers, print the result, exit
    std::string load;           // phase 6: a save file to open at start (the world scene)
    bool autoplay = false;      // drive the player through a fixed demo (walk, jet, blast, grapple) for smoke runs
    bool noLight = false;       // start with the lighting off (F4 toggles)
    bool lightCpu = false;      // light on the CPU reference even when the GPU pass works
    bool lightSelfTest = false; // compare the GPU and CPU lighting on a fixed scene, print the result, exit
    bool oldLook = false;       // #477: start with liquids drawn as pixels (F10 flips)
    bool oldMotion = false;     // #477: start with the phase 1 powder / liquid rules (F10 flips)
    bool lockstep = false;      // exactly one sim tick per frame (comparable screenshots)
    bool oldFlames = false;     // #480: start with flames as pixels (F11 flips)
    bool noRelief = false;      // #481: start without the surface relief / wet look (F12 flips)
    bool at = false;            // --at x,y: put the player (feet) there at start (screenshots of a room)
    float atX = 0, atY = 0;
};

class App {
public:
    explicit App(AppOptions opts);
    ~App();
    bool init();
    int run();
    int gasSelfTest(); // 0 when the GPU solver matches the CPU reference
    int lightSelfTest(); // 0 when the GPU lighting matches the CPU reference

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
    // phase 6: the streamed procedural world, save/load
    void startWorld();
    void followWorld();
    bool saveWorld();
    bool loadWorld();
    void drawWorldUi();
    void drawElectricUi();
    void drawElectric();   // the current / potential overlay (M)
    void toggleSwitchAt(int x, int y);
    void autoShowcase();   // --scene showcase --autoplay: closes the switches, logs stats (smoke runs)
    void autoCircuits();   // --scene circuits --autoplay: flips the switches, calls lightning (smoke runs)
    bool circuitsScene_ = false;
    bool showcaseScene_ = false; // the streamed world with the showcase building laid over it
    int  circuitTick_ = 0;
    void drawWorld();
    void drawUi();
    void drawTitleCard(); // the game's name over the first seconds, fading out (panels hidden or not)
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
    std::unique_ptr<ElectricSystem> electric_;
    bool showElectric_ = false; // overlay: current (bright) or potential (hue)
    int  electricView_ = 0;     // 0 current, 1 potential
    bool particlesOn_ = true;
    std::unique_ptr<Player> player_;
    std::unique_ptr<WorldGen> gen_;
    std::unique_ptr<Showcase> showcase_;
    std::unique_ptr<Streamer> stream_;
    bool streaming_ = false;     // the world scene: the window follows the player (or the camera)
    std::string savePath_ = "notita_world.sav";
    std::string worldMsg_;
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
    // ---- phase 6: lighting (AppLight.cpp) ----
    void initLighting();      // material light tables + the GPU pass (after the renderer)
    bool lightingActive() const; // on, and not in the temperature / velocity / pressure views
    void prepareLight();      // after drawWorld: fill the light inputs; the CPU path lights the pixels here
    bool recordLight(SDL_GPUCommandBuffer* cmd); // GPU path: writes the view texture, true when it did
    void lightScreenshot();   // GPU path: read the lit view back into the pixels before a screenshot
    void drawLightUi();
    std::unique_ptr<GpuLight> gpuLight_; // null when the GPU pass is unavailable
    std::string gpuLightError_;
    CpuLight cpuLight_;
    LightInputs lightIn_;
    LightParams lightParams_;  // the GPU settings
    LightParams lightParamsCpu_; // coarser cells / fewer rays so the CPU path keeps up
    bool lightOn_ = true, useGpuLight_ = true;
    double lightInputMs_ = 0.0, lightGpuMs_ = 0.0;
    std::vector<std::array<float, 4>> matEmit_; // per material: emitted rgb, opacity per pixel
    std::array<float, 4 * 256> heatEmit_{};     // incandescence rgb by temperature (400 + 5 C steps)
    std::vector<std::uint32_t> matPacked_;      // matEmit_ packed (cold, not burning)
    std::vector<std::uint8_t> gasOpacity_;      // per gas cell over the view, scratch

    // ---- #477: liquid look (AppLiquid.cpp) and the old/new motion toggle ----
    static constexpr int kLiquidRamp = 64;     // depth steps of a liquid's colour ramp
    void initLiquid();        // after initLighting: material looks + the GPU surface pass
    bool liquidActive() const; // new look on, and not in the temperature / velocity / pressure / electric views
    void shadeLiquids();      // in drawWorld: depth colours for liquid pixels + the per-pixel info
    void markLiquidParticle(int vx, int vy, MaterialId m);
    void clearLiquidInfo(int vx, int vy);
    int  liquidScale() const;
    bool recordLiquid(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swap, bool lit, bool present = true); // true when it drew the frame
    bool liquidScreenshot(const std::string& path);
    void setNewLookAndMotion(bool on);
    void drawLiquidUi();
    static bool writeBmp(const std::string& path, const std::vector<std::uint32_t>& px, int w, int h);
    std::unique_ptr<GpuLiquid> gpuLiquid_; // null when the GPU pass is unavailable
    std::string gpuLiquidError_;
    std::vector<std::uint8_t> liquidLook_;     // per material: look slot (0 = not a liquid)
    std::vector<std::uint32_t> liquidRamp_;    // per material x kLiquidRamp depth colours
    std::vector<std::uint32_t> liquidInfo_;    // per view pixel, see liquid.hlsl
    std::array<LiquidLook, kLiquidLooks> looks_{};
    bool newLook_ = true, newMotion_ = true, smoothPowder_ = true, calmLiquid_ = true, liquidWobble_ = true;
    bool liquidDrawn_ = false;
    float liquidTime_ = 0.0f;

    // ---- #481: surface relief and wet look on textured materials (AppTexture.cpp) ----
    void rebuildSurfaceKinds(); // per material: open / dense (solid, powder) / liquid
    void applyRelief();         // in drawWorld, after the cells are copied
    void drawTextureUi();
    bool relief_ = true;
    float reliefStrength_ = 1.0f, wetness_ = 1.0f;
    std::vector<std::uint8_t> matSurface_; // per material
    std::vector<std::uint8_t> viewMask_;   // per view pixel, scratch

    // ---- #480: flames as hot glowing gas on the grid (AppFlame.cpp) ----
    void initFlame();         // after initLiquid: tints + the GPU flame pass
    void setNewFlames(bool on);
    bool flameActive() const; // new flames on, the grid on, and the normal view
    void buildFlameCells();   // in drawWorld: the grid fields over the view
    void flameAt(int x, int y, float out[3]) const;
    void drawFlamesCpu();     // no GPU pass: flames into the view pixels
    void addFlameLight();     // in prepareLight: the flames' emission
    bool recordFlame(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swap, bool lit, bool liquid); // true when it drew the frame
    bool flameScreenshot(const std::string& path);
    void drawFlameUi();
    std::unique_ptr<GpuFlame> gpuFlame_; // null when the GPU pass is unavailable
    std::string gpuFlameError_;
    bool newFlames_ = true, flameAny_ = false, flameDrawn_ = false;
    std::vector<FlameCell> flameCells_;
    std::vector<float> flameEmit_;           // per cell rgb: flame colour x intensity (no noise)
    int flameCW_ = 0, flameCH_ = 0, flameI0_ = 0, flameJ0_ = 0;
    float flameOffX_ = 0, flameOffY_ = 0;
    FlameLook flameLook_;
    float flameLightGain_ = 0.45f;            // the flames' light (with the GPU pass drawing the flame itself)
    std::vector<std::uint8_t> flameTintIdx_; // per material: tint slot (0 = none)
    std::array<std::array<float, 4>, kFlameTints> flameTints_{};

    std::array<float, 120> tickHistory_{};
    int tickHistoryPos_ = 0;
    double frameMs_ = 0.0;
};

} // namespace notita
