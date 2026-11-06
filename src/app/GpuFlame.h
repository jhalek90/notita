#pragma once
// The flame pass on the GPU (#480, src/app/shaders/flame.hlsl). Flames are
// hot, reacting gas on the gas grid; this pass draws them from the grid
// fields over the view (temperature, burn rate, soot, fuel tint), smooth and
// upsampled to the output resolution with a cubic B-spline, a blackbody
// colour ramp, additive glow, soot darkening and a noise-driven flicker.
// Reads the finished scene (the lit view texture, or the liquid pass output)
// and writes its own output texture, which the Renderer blits instead.
// Purely cosmetic: nothing comes back to the sim.
#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace notita {

constexpr int kFlameTints = 16; // tint 0 = none (plain blackbody)

// Per view cell, laid out like float4 FlameCell in flame.hlsl.
struct FlameCell {
    float temperature = 20.0f; // C
    float burn = 0.0f;         // energy released per tick (smoothed)
    float soot = 0.0f;         // soot / smoke darkness
    float tint = 0.0f;         // tint index + amount (index + 0..0.99)
};

struct FlameLook {
    float gain = 0.9f;     // flame brightness
    float smoke = 1.0f;    // soot darkening strength
    float flicker = 1.0f;  // noise warp and flicker strength
    float minTemp = 480.0f; // C where gas starts to glow
};

class GpuFlame {
public:
    ~GpuFlame() { shutdown(); }
    bool init(SDL_GPUDevice* device, const std::string& shaderDir);
    void shutdown();
    // scene: sw x sh texture readable by compute (the view or the liquid output).
    // cells: cw x ch grid cells over the view; grid coordinate (cell centres at
    // integers) of sim pixel x is (x + 0.5) / 4 + offX; (orgX, orgY) is the world cell
    // of buffer cell (0, 0), so the noise stays put when the view moves. Output is (w*scale) x (h*scale).
    bool record(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* scene, int sw, int sh, const std::vector<FlameCell>& cells,
                int cw, int ch, float offX, float offY, float orgX, float orgY, int w, int h, int scale, float time, const FlameLook& look,
                const std::array<std::array<float, 4>, kFlameTints>& tints);
    SDL_GPUTexture* output() const { return out_; }
    int outW() const { return ow_; }
    int outH() const { return oh_; }
    bool readBack(std::vector<std::uint32_t>& px);
    const std::string& error() const { return error_; }

private:
    bool ensure(int ow, int oh, int cells);
    void release();

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUComputePipeline* pipe_ = nullptr;
    SDL_GPUTexture* out_ = nullptr;
    SDL_GPUBuffer* cells_ = nullptr;
    SDL_GPUTransferBuffer* up_ = nullptr;
    SDL_GPUTransferBuffer* down_ = nullptr;
    int ow_ = 0, oh_ = 0, ncells_ = 0;
    std::string error_;
};

} // namespace notita
