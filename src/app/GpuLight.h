#pragma once
// The view lighting on the GPU (SDL3 GPU compute, src/app/shaders/light.hlsl).
// Same kernels and order as CpuLight (src/light/Lighting.h). Each frame
// uploads the packed inputs, runs the kernels (one compute pass per step, as
// SDL requires) and lets the composite write straight into the texture the
// Renderer blits, so nothing comes back to the CPU unless asked (self-test,
// screenshots).
#include "light/Lighting.h"

#include <SDL3/SDL.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace notita {

class GpuLight {
public:
    ~GpuLight() { shutdown(); }
    // Loads light_<kernel>.dxil from shaderDir. False (and error()) when the
    // device has no DXIL support or a shader is missing: use the CPU path.
    bool init(SDL_GPUDevice* device, const std::string& shaderDir);
    void shutdown();
    // Uploads in and records the kernels into cmd; the composite writes target
    // (in.w x in.h RGBA8 with COMPUTE_STORAGE_WRITE). False when nothing was recorded.
    bool record(SDL_GPUCommandBuffer* cmd, const LightInputs& in, const LightParams& p, SDL_GPUTexture* target);
    // record, then download target and wait for it (self-test, screenshots).
    bool runSync(const LightInputs& in, const LightParams& p, SDL_GPUTexture* target, std::vector<std::uint32_t>& out);
    // Kernels only, reps times in one command buffer on the last uploaded
    // inputs; returns GPU wall ms per pass (no upload or download in it).
    double benchmark(const LightParams& p, SDL_GPUTexture* target, int reps);
    const std::string& error() const { return error_; }

private:
    enum Kernel { kCells, kMip, kSky, kTrace, kTent, kBlur, kComposite, kKernelCount };
    enum Buf {
        bAlbedo, bEmit, bTopSky, bDirs, bSched, bMip, bSkyTop, bSky, bTrace, bLight,
        bTmp, bBloomN, bTmpFar, bBloomF, kBufCount
    };
    bool ensure(int w, int h, const LightParams& p);
    void releaseBuffers();
    void kernels(SDL_GPUCommandBuffer* cmd, const LightParams& p, SDL_GPUTexture* target);
    void dispatch(SDL_GPUCommandBuffer* cmd, Kernel k, std::initializer_list<Buf> ro, std::initializer_list<Buf> rw,
                  SDL_GPUTexture* rwTex, int nx, int ny, const void* uniforms);

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUComputePipeline* pipes_[kKernelCount]{};
    SDL_GPUBuffer* bufs_[kBufCount]{};
    Uint32 sizes_[kBufCount]{};
    SDL_GPUTransferBuffer* up_ = nullptr;
    SDL_GPUTransferBuffer* down_ = nullptr;
    Uint32 upSize_ = 0;
    int w_ = -1, h_ = -1, cell_ = -1, dirs_ = -1, range_ = -1;
    int cw_ = 0, ch_ = 0;
    LightLayout lay_;
    std::vector<std::int32_t> dirTable_, sched_;
    std::string error_;
};

} // namespace notita
