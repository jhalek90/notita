#pragma once
// Presents the sim view: a CPU-filled RGBA8 buffer is uploaded to a GPU
// texture each frame and blitted (nearest filter, integer scale when it fits)
// onto the swapchain. Later phases add GPU passes (gas grid, heat, lighting)
// that read from / write to this texture before the blit.
#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

namespace notita {

struct Viewport {
    float x = 0, y = 0, w = 0, h = 0; // in swapchain pixels
    float scale = 1;                  // screen pixels per sim pixel
};

class Renderer {
public:
    bool init(SDL_GPUDevice* device, int width, int height);
    void shutdown();

    int width() const { return width_; }
    int height() const { return height_; }
    std::vector<std::uint32_t>& pixels() { return pixels_; } // RGBA8, row-major

    // Fit the sim view into a target of tw x th pixels.
    Viewport fit(std::uint32_t tw, std::uint32_t th) const;

    // Records upload + blit into cmd. Call before the UI render pass.
    void record(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, std::uint32_t tw, std::uint32_t th,
                const Viewport& vp);

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTexture* texture_ = nullptr;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    int width_ = 0, height_ = 0;
    std::vector<std::uint32_t> pixels_;
};

} // namespace notita
