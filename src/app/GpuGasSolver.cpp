#include "GpuGasSolver.h"

#include <chrono>
#include <cstring>

namespace notita {

namespace {

// Matches cbuffer Params in gas.hlsl (16-byte rows).
struct Uniforms {
    int w, h, ns, iters;
    float buoyHeat, buoyDens, vort, damping;
    float maxSpeed, diffusion, airCap;
    int flatCount;
    float light[kMaxGasSpecies];
    float cap[kMaxGasSpecies];
    int flat[GasSolveParams::kMaxFlat][4];
};
static_assert(sizeof(Uniforms) == 176, "keep Uniforms in step with gas.hlsl");

struct KernelInfo {
    const char* file;
    Uint32 ro, rw;
};
constexpr KernelInfo kKernels[] = {
    {"gas_curl.dxil", 3, 1},       {"gas_forces.dxil", 4, 2}, {"gas_advect.dxil", 3, 2},
    {"gas_divergence.dxil", 4, 1}, {"gas_jacobi.dxil", 3, 1}, {"gas_project.dxil", 4, 2},
    {"gas_scale.dxil", 2, 1},      {"gas_scalars.dxil", 6, 2},
};

Uint32 groups(int n) { return static_cast<Uint32>((n + 7) / 8); }

} // namespace

bool GpuGasSolver::init(SDL_GPUDevice* device, const std::string& shaderDir) {
    device_ = device;
    if (!(SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_DXIL)) {
        error_ = "the GPU device does not take DXIL shaders (driver " + std::string(SDL_GetGPUDeviceDriver(device)) + ")";
        return false;
    }
    for (int k = 0; k < kKernelCount; ++k) {
        const std::string path = shaderDir + kKernels[k].file;
        size_t size = 0;
        void* code = SDL_LoadFile(path.c_str(), &size);
        if (!code) {
            error_ = "missing shader " + path;
            shutdown();
            return false;
        }
        SDL_GPUComputePipelineCreateInfo ci{};
        ci.code = static_cast<const Uint8*>(code);
        ci.code_size = size;
        ci.entrypoint = "main";
        ci.format = SDL_GPU_SHADERFORMAT_DXIL;
        ci.num_readonly_storage_buffers = kKernels[k].ro;
        ci.num_readwrite_storage_buffers = kKernels[k].rw;
        ci.num_uniform_buffers = 1;
        ci.threadcount_x = 8;
        ci.threadcount_y = 8;
        ci.threadcount_z = 1;
        pipes_[k] = SDL_CreateGPUComputePipeline(device, &ci);
        SDL_free(code);
        if (!pipes_[k]) {
            error_ = std::string("compute pipeline ") + kKernels[k].file + ": " + SDL_GetError();
            shutdown();
            return false;
        }
    }
    return true;
}

void GpuGasSolver::releaseBuffers() {
    if (!device_) return;
    for (auto& b : bufs_)
        if (b) { SDL_ReleaseGPUBuffer(device_, b); b = nullptr; }
    if (up_) SDL_ReleaseGPUTransferBuffer(device_, up_);
    if (down_) SDL_ReleaseGPUTransferBuffer(device_, down_);
    up_ = down_ = nullptr;
    w_ = h_ = ns_ = -1;
}

void GpuGasSolver::shutdown() {
    if (!device_) return;
    releaseBuffers();
    for (auto& p : pipes_)
        if (p) { SDL_ReleaseGPUComputePipeline(device_, p); p = nullptr; }
    device_ = nullptr;
}

bool GpuGasSolver::ensure(const GasFields& f) {
    if (f.w == w_ && f.h == h_ && f.species == ns_) return true;
    releaseBuffers();
    const Uint32 n = static_cast<Uint32>(f.cells()) * 4u;
    const Uint32 nu = static_cast<Uint32>(f.u.size()) * 4u, nv = static_cast<Uint32>(f.v.size()) * 4u;
    const Uint32 nm = static_cast<Uint32>(f.mass.size()) * 4u;
    const Uint32 size[kBufCount] = {n, nu, nv, nu, nv, n, n, n, n, n, n, nm, nm, n, n};
    for (int b = 0; b < kBufCount; ++b) {
        SDL_GPUBufferCreateInfo bi{};
        bi.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
        bi.size = size[b];
        sizes_[b] = size[b];
        bufs_[b] = SDL_CreateGPUBuffer(device_, &bi);
        if (!bufs_[b]) { error_ = SDL_GetError(); releaseBuffers(); return false; }
    }
    // upload: open, u, v, p, src, mass, heat; download: u, v, p, mass, heat
    upSize_ = n + nu + nv + n + n + nm + n;
    downSize_ = nu + nv + n + nm + n;
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = upSize_;
    up_ = SDL_CreateGPUTransferBuffer(device_, &ti);
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = downSize_;
    down_ = SDL_CreateGPUTransferBuffer(device_, &ti);
    if (!up_ || !down_) { error_ = SDL_GetError(); releaseBuffers(); return false; }
    w_ = f.w;
    h_ = f.h;
    ns_ = f.species;
    return true;
}

void GpuGasSolver::dispatch(SDL_GPUCommandBuffer* cmd, Kernel k, std::initializer_list<Buf> ro,
                            std::initializer_list<Buf> rw, int nx, int ny) {
    SDL_GPUStorageBufferReadWriteBinding rwb[4]{};
    Uint32 nrw = 0;
    for (Buf b : rw) rwb[nrw++].buffer = bufs_[b]; // cycle = false: keep the contents
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, rwb, nrw);
    SDL_BindGPUComputePipeline(pass, pipes_[k]);
    SDL_PushGPUComputeUniformData(cmd, 0, uniforms_, sizeof(Uniforms));
    SDL_GPUBuffer* rob[8]{};
    Uint32 nro = 0;
    for (Buf b : ro) rob[nro++] = bufs_[b];
    SDL_BindGPUComputeStorageBuffers(pass, 0, rob, nro);
    SDL_DispatchGPUCompute(pass, groups(nx), groups(ny), 1);
    SDL_EndGPUComputePass(pass);
}

void GpuGasSolver::solve(GasFields& f, const GasSolveParams& sp) {
    const auto t0 = std::chrono::steady_clock::now();
    if (f.w <= 0 || f.h <= 0 || !ensure(f)) return;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) return;

    struct Part { Buf buf; const void* data; Uint32 size; };
    const Part ups[] = {
        {bOpen, f.open.data(), sizes_[bOpen]}, {bU, f.u.data(), sizes_[bU]},   {bV, f.v.data(), sizes_[bV]},
        {bP, f.p.data(), sizes_[bP]},          {bSrc, f.src.data(), sizes_[bSrc]},
        {bMass, f.mass.data(), sizes_[bMass]}, {bHeat, f.heat.data(), sizes_[bHeat]},
    };
    auto* dst = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device_, up_, true));
    if (!dst) { SDL_CancelGPUCommandBuffer(cmd); return; }
    Uint32 off = 0;
    for (const Part& p : ups) { std::memcpy(dst + off, p.data, p.size); off += p.size; }
    SDL_UnmapGPUTransferBuffer(device_, up_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    off = 0;
    for (const Part& p : ups) {
        SDL_GPUTransferBufferLocation src{up_, off};
        SDL_GPUBufferRegion reg{bufs_[p.buf], 0, p.size};
        SDL_UploadToGPUBuffer(copy, &src, &reg, false);
        off += p.size;
    }
    SDL_EndGPUCopyPass(copy);

    Uniforms un{};
    un.w = f.w; un.h = f.h; un.ns = f.species; un.iters = sp.pressureIters;
    un.buoyHeat = sp.buoyancyHeat; un.buoyDens = sp.buoyancyDensity; un.vort = sp.vorticity; un.damping = sp.damping;
    un.maxSpeed = sp.maxSpeed; un.diffusion = sp.diffusion; un.airCap = sp.airCap;
    for (int k = 0; k < kMaxGasSpecies; ++k) { un.light[k] = sp.light[k]; un.cap[k] = sp.cap[k]; }
    un.flatCount = sp.flatCount;
    for (int k = 0; k < GasSolveParams::kMaxFlat; ++k)
        for (int q = 0; q < 4; ++q) un.flat[k][q] = sp.flat[k][q];
    uniforms_ = &un;

    const int W = f.w, H = f.h;
    dispatch(cmd, kCurl, {bOpen, bU, bV}, {bCurl}, W, H);
    dispatch(cmd, kForces, {bOpen, bCurl, bMass, bHeat}, {bU, bV}, W + 1, H + 1);
    dispatch(cmd, kAdvect, {bOpen, bU, bV}, {bU2, bV2}, W + 1, H + 1);
    dispatch(cmd, kDivergence, {bOpen, bU2, bV2, bSrc}, {bDiv}, W, H);
    Buf pin = bP, pout = bP2;
    for (int it = 0; it < sp.pressureIters; ++it) {
        dispatch(cmd, kJacobi, {bOpen, bDiv, pin}, {pout}, W, H);
        std::swap(pin, pout);
    }
    dispatch(cmd, kProject, {bOpen, bU2, bV2, pin}, {bU, bV}, W + 1, H + 1);
    dispatch(cmd, kScale, {bU, bV}, {bScale}, W, H);
    dispatch(cmd, kScalars, {bOpen, bU, bV, bScale, bMass, bHeat}, {bMass2, bHeat2}, W, H);

    const Part downs[] = {
        {bU, f.u.data(), sizes_[bU]}, {bV, f.v.data(), sizes_[bV]}, {pin, f.p.data(), sizes_[bP]},
        {bMass2, f.mass.data(), sizes_[bMass]}, {bHeat2, f.heat.data(), sizes_[bHeat]},
    };
    copy = SDL_BeginGPUCopyPass(cmd);
    off = 0;
    for (const Part& p : downs) {
        SDL_GPUBufferRegion reg{bufs_[p.buf], 0, p.size};
        SDL_GPUTransferBufferLocation to{down_, off};
        SDL_DownloadFromGPUBuffer(copy, &reg, &to);
        off += p.size;
    }
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) return;
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);

    if (const auto* srcp = static_cast<const std::uint8_t*>(SDL_MapGPUTransferBuffer(device_, down_, false))) {
        off = 0;
        for (const Part& p : downs) { std::memcpy(const_cast<void*>(p.data), srcp + off, p.size); off += p.size; }
        SDL_UnmapGPUTransferBuffer(device_, down_);
    }
    lastMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace notita
