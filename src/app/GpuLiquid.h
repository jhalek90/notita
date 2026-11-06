#pragma once
// The liquid surface pass on the GPU (#477, src/app/shaders/liquid.hlsl).
// Reads the coloured (and lit) view texture plus a per-pixel liquid info
// buffer and writes an output texture S times the view size, which the
// Renderer then blits instead of the view. Purely cosmetic: nothing comes
// back to the sim.
#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace notita {

constexpr int kLiquidLooks = 16; // look 0 = not liquid

struct LiquidLook {
    float shine[4] = {1, 1, 1, 0.5f}; // surface line colour (linear 0..1) and strength
    float foam[4] = {1, 1, 1, 0.8f};  // foam colour and strength
    float wobble = 0.35f;             // surface wave amplitude, sim px
    float shimmer = 0.25f;            // interior refraction offset, sim px
    float glint = 0.6f;               // moving highlight on the surface line
};

class GpuLiquid {
public:
    ~GpuLiquid() { shutdown(); }
    bool init(SDL_GPUDevice* device, const std::string& shaderDir);
    void shutdown();
    // Records the pass: scene is the w x h view texture (readable by compute),
    // info w*h words (see liquid.hlsl). Output is (w*scale) x (h*scale).
    bool record(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* scene, const std::vector<std::uint32_t>& info, int w, int h,
                int scale, float time, bool wobble, const std::array<LiquidLook, kLiquidLooks>& looks);
    SDL_GPUTexture* output() const { return out_; }
    int outW() const { return ow_; }
    int outH() const { return oh_; }
    // Downloads the last output (call after the command buffer that recorded it was submitted).
    bool readBack(std::vector<std::uint32_t>& px);
    const std::string& error() const { return error_; }

private:
    bool ensure(int w, int h, int scale);
    void release();

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUComputePipeline* pipe_ = nullptr;
    SDL_GPUTexture* out_ = nullptr;
    SDL_GPUBuffer* info_ = nullptr;
    SDL_GPUTransferBuffer* up_ = nullptr;
    SDL_GPUTransferBuffer* down_ = nullptr;
    int w_ = 0, h_ = 0, s_ = 0, ow_ = 0, oh_ = 0;
    std::string error_;
};

} // namespace notita
