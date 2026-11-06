#include "Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace notita {

bool Renderer::init(SDL_GPUDevice* device, int width, int height) {
    device_ = device;
    width_ = width;
    height_ = height;
    pixels_.assign(static_cast<std::size_t>(width) * height, 0xFF000000u);

    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    // compute-writable too when the device allows it, so the lighting composite can write it in place
    storage_ = SDL_GPUTextureSupportsFormat(device_, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D,
                                            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (storage_ ? SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE : 0);
    // and readable by compute (the liquid surface pass reads the lit view)
    readable_ = SDL_GPUTextureSupportsFormat(device_, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D,
                                             tci.usage | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ);
    if (readable_) tci.usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ;
    tci.width = static_cast<Uint32>(width);
    tci.height = static_cast<Uint32>(height);
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    texture_ = SDL_CreateGPUTexture(device_, &tci);
    if (!texture_) return false;

    SDL_GPUTransferBufferCreateInfo bci{};
    bci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    bci.size = static_cast<Uint32>(pixels_.size() * sizeof(std::uint32_t));
    upload_ = SDL_CreateGPUTransferBuffer(device_, &bci);
    return upload_ != nullptr;
}

void Renderer::shutdown() {
    if (!device_) return;
    if (upload_) SDL_ReleaseGPUTransferBuffer(device_, upload_);
    if (texture_) SDL_ReleaseGPUTexture(device_, texture_);
    upload_ = nullptr;
    texture_ = nullptr;
    device_ = nullptr;
}

Viewport Renderer::fit(std::uint32_t tw, std::uint32_t th) const {
    Viewport vp;
    const float sx = static_cast<float>(tw) / static_cast<float>(width_);
    const float sy = static_cast<float>(th) / static_cast<float>(height_);
    float s = std::min(sx, sy);
    if (s >= 1.0f) s = std::floor(s); // crisp integer scaling whenever the window allows
    vp.scale = std::max(s, 0.01f);
    vp.w = static_cast<float>(width_) * vp.scale;
    vp.h = static_cast<float>(height_) * vp.scale;
    vp.x = std::floor((static_cast<float>(tw) - vp.w) * 0.5f);
    vp.y = std::floor((static_cast<float>(th) - vp.h) * 0.5f);
    return vp;
}

void Renderer::upload(SDL_GPUCommandBuffer* cmd) {
    if (void* dst = SDL_MapGPUTransferBuffer(device_, upload_, true)) {
        std::memcpy(dst, pixels_.data(), pixels_.size() * sizeof(std::uint32_t));
        SDL_UnmapGPUTransferBuffer(device_, upload_);
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = upload_;
    src.pixels_per_row = static_cast<Uint32>(width_);
    src.rows_per_layer = static_cast<Uint32>(height_);
    SDL_GPUTextureRegion region{};
    region.texture = texture_;
    region.w = static_cast<Uint32>(width_);
    region.h = static_cast<Uint32>(height_);
    region.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &region, true);
    SDL_EndGPUCopyPass(copy);
}

void Renderer::blit(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* src, int sw, int sh, SDL_GPUTexture* target, const Viewport& vp) {
    SDL_GPUBlitInfo blit{};
    blit.source.texture = src;
    blit.source.w = static_cast<Uint32>(sw);
    blit.source.h = static_cast<Uint32>(sh);
    blit.destination.texture = target;
    blit.destination.x = static_cast<Uint32>(std::max(vp.x, 0.0f));
    blit.destination.y = static_cast<Uint32>(std::max(vp.y, 0.0f));
    blit.destination.w = static_cast<Uint32>(vp.w);
    blit.destination.h = static_cast<Uint32>(vp.h);
    blit.load_op = SDL_GPU_LOADOP_CLEAR;
    blit.clear_color = SDL_FColor{0.02f, 0.02f, 0.03f, 1.0f};
    // 1:1 (or an integer multiple) stays crisp; anything else is filtered
    const bool exact = static_cast<int>(vp.w) % sw == 0 && static_cast<int>(vp.h) % sh == 0;
    blit.filter = exact ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cmd, &blit);
}

void Renderer::record(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, std::uint32_t tw, std::uint32_t th,
                      const Viewport& vp, bool doUpload) {
    (void)tw;
    (void)th;
    if (doUpload) upload(cmd);
    SDL_GPUBlitInfo blit{};
    blit.source.texture = texture_;
    blit.source.w = static_cast<Uint32>(width_);
    blit.source.h = static_cast<Uint32>(height_);
    blit.destination.texture = target;
    blit.destination.x = static_cast<Uint32>(std::max(vp.x, 0.0f));
    blit.destination.y = static_cast<Uint32>(std::max(vp.y, 0.0f));
    blit.destination.w = static_cast<Uint32>(vp.w);
    blit.destination.h = static_cast<Uint32>(vp.h);
    blit.load_op = SDL_GPU_LOADOP_CLEAR;
    blit.clear_color = SDL_FColor{0.02f, 0.02f, 0.03f, 1.0f};
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(cmd, &blit);
}

} // namespace notita
