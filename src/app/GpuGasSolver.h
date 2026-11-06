#pragma once
// The gas grid math on the GPU (SDL3 GPU compute, src/app/shaders/gas.hlsl).
// Same kernels and order as CpuGasSolver. Each tick uploads the grid,
// runs the kernels (one compute pass per dependent step, as SDL requires),
// downloads the result and waits for it, so GasGrid can couple with the
// pixels right away. Mass stays integer-exact on the GPU too: every face
// flux is computed by both cells with the same expression.
#include "sim/GasGrid.h"

#include <SDL3/SDL.h>

#include <string>

namespace notita {

class GpuGasSolver : public GasSolver {
public:
    ~GpuGasSolver() override { shutdown(); }
    // Loads gas_<kernel>.dxil from shaderDir. False (and error()) when the
    // device has no DXIL support or a shader is missing: use the CPU solver.
    bool init(SDL_GPUDevice* device, const std::string& shaderDir);
    void shutdown();
    void solve(GasFields& f, const GasSolveParams& sp) override;
    const char* name() const override { return "gpu"; }
    const std::string& error() const { return error_; }
    double lastMs() const { return lastMs_; } // upload + kernels + download, wall time

private:
    enum Kernel { kCurl, kForces, kAdvect, kDivergence, kJacobi, kProject, kScale, kScalars, kKernelCount };
    enum Buf { bOpen, bU, bV, bU2, bV2, bP, bP2, bSrc, bDiv, bCurl, bScale, bMass, bMass2, bHeat, bHeat2, kBufCount };
    bool ensure(const GasFields& f);
    void releaseBuffers();
    void dispatch(SDL_GPUCommandBuffer* cmd, Kernel k, std::initializer_list<Buf> ro, std::initializer_list<Buf> rw,
                  int nx, int ny);

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUComputePipeline* pipes_[kKernelCount]{};
    SDL_GPUBuffer* bufs_[kBufCount]{};
    Uint32 sizes_[kBufCount]{};
    SDL_GPUTransferBuffer* up_ = nullptr;
    SDL_GPUTransferBuffer* down_ = nullptr;
    Uint32 upSize_ = 0, downSize_ = 0;
    int w_ = -1, h_ = -1, ns_ = -1;
    std::string error_;
    double lastMs_ = 0.0;
    const void* uniforms_ = nullptr; // this solve's parameter block
};

} // namespace notita
