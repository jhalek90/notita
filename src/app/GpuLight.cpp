#include "GpuLight.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace notita {

namespace {

// Matches cbuffer Params in light.hlsl (16-byte rows).
struct Uniforms {
    int w, h, cw, ch;
    int c, d, nSched, level;
    int srcW, srcH, dx, dy;
    int radius, srcOff, range, pad0;
    int mipW[8], mipH[8], mipOff[8];
    float gain, ambient, sky, skyGain;
    float skyR, skyG, skyB, bloomN;
    float bloomF, invRange, invDirs, invC;
    float invCF, emitK, haze, pad1;
    float wt[12];
};
static_assert(sizeof(Uniforms) == 272, "keep Uniforms in step with light.hlsl");

struct KernelInfo {
    const char* file;
    Uint32 ro, rw, rwTex;
};
constexpr KernelInfo kKernels[] = {
    {"light_cells.dxil", 1, 1, 0}, {"light_mip.dxil", 0, 1, 0},  {"light_sky.dxil", 2, 2, 0},
    {"light_trace.dxil", 5, 1, 0}, {"light_tent.dxil", 1, 1, 0}, {"light_blur.dxil", 1, 1, 0},
    {"light_composite.dxil", 5, 0, 1},
};

Uint32 groups(int n) { return static_cast<Uint32>((n + 7) / 8); }

Uniforms makeUniforms(int w, int h, int cw, int ch, const LightLayout& lay, int nSched, const LightParams& p) {
    Uniforms u{};
    const int C = std::max(1, p.cellPx), D = std::max(1, p.dirs), R = std::max(1, p.range);
    u.w = w; u.h = h; u.cw = cw; u.ch = ch;
    u.c = C; u.d = D; u.nSched = nSched; u.range = R;
    for (int l = 0; l < kLightLevels; ++l) { u.mipW[l] = lay.w[l]; u.mipH[l] = lay.h[l]; u.mipOff[l] = lay.off[l]; }
    u.radius = std::clamp(p.bloomRadius, 0, kBlurMaxRadius);
    u.gain = p.gain; u.ambient = p.ambient; u.sky = p.sky; u.skyGain = p.skyGain;
    u.skyR = p.skyR; u.skyG = p.skyG; u.skyB = p.skyB;
    u.bloomN = p.bloom * p.bloomNear; u.bloomF = p.bloom * p.bloomFar;
    // the same float expressions as CpuLight, so both sides use identical constants
    u.invRange = 1.0f / static_cast<float>(R + 1);
    u.invDirs = 1.0f / static_cast<float>(D);
    u.invC = 1.0f / static_cast<float>(C);
    u.invCF = 1.0f / static_cast<float>(C << kBloomLevel);
    u.emitK = kEmitMax / 255.0f;
    u.haze = p.haze;
    gaussWeights(p.bloomRadius, p.bloomSigma, u.wt);
    return u;
}

} // namespace

bool GpuLight::init(SDL_GPUDevice* device, const std::string& shaderDir) {
    device_ = device;
    if (!(SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_DXIL)) {
        error_ = "the GPU device does not take DXIL shaders (driver " + std::string(SDL_GetGPUDeviceDriver(device)) + ")";
        return false;
    }
    if (!SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE)) {
        error_ = "RGBA8 storage textures are not supported";
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
        ci.num_readwrite_storage_textures = kKernels[k].rwTex;
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

void GpuLight::releaseBuffers() {
    if (!device_) return;
    for (auto& b : bufs_)
        if (b) { SDL_ReleaseGPUBuffer(device_, b); b = nullptr; }
    if (up_) SDL_ReleaseGPUTransferBuffer(device_, up_);
    if (down_) SDL_ReleaseGPUTransferBuffer(device_, down_);
    up_ = down_ = nullptr;
    w_ = h_ = cell_ = dirs_ = range_ = -1;
}

void GpuLight::shutdown() {
    if (!device_) return;
    releaseBuffers();
    for (auto& p : pipes_)
        if (p) { SDL_ReleaseGPUComputePipeline(device_, p); p = nullptr; }
    device_ = nullptr;
}

bool GpuLight::ensure(int w, int h, const LightParams& p) {
    const int C = std::max(1, p.cellPx), D = std::max(1, p.dirs), R = std::max(1, p.range);
    if (w == w_ && h == h_ && C == cell_ && D == dirs_ && R == range_) return true;
    releaseBuffers();
    cw_ = (w + C - 1) / C;
    ch_ = (h + C - 1) / C;
    lay_ = lightLayout(cw_, ch_);
    dirTable_ = lightDirections(D);
    sched_ = lightSchedule(D, R);
    const Uint32 px = static_cast<Uint32>(w) * h * 4u, nc = static_cast<Uint32>(cw_) * ch_ * 4u;
    const Uint32 nb = static_cast<Uint32>(lay_.w[kBloomLevel]) * lay_.h[kBloomLevel] * 16u;
    const Uint32 size[kBufCount] = {px, px, static_cast<Uint32>(w) * 4u, static_cast<Uint32>(dirTable_.size()) * 4u,
                                    static_cast<Uint32>(sched_.size()) * 4u, static_cast<Uint32>(lay_.total) * 16u,
                                    static_cast<Uint32>(cw_) * 4u, nc, nc * 4u, nc * 4u,
                                    nc * 4u, nc * 4u, nb, nb};
    for (int b = 0; b < kBufCount; ++b) {
        SDL_GPUBufferCreateInfo bi{};
        bi.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
        bi.size = size[b];
        sizes_[b] = size[b];
        bufs_[b] = SDL_CreateGPUBuffer(device_, &bi);
        if (!bufs_[b]) { error_ = SDL_GetError(); releaseBuffers(); return false; }
    }
    upSize_ = sizes_[bAlbedo] + sizes_[bEmit] + sizes_[bTopSky] + sizes_[bDirs] + sizes_[bSched];
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = upSize_;
    up_ = SDL_CreateGPUTransferBuffer(device_, &ti);
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = px;
    down_ = SDL_CreateGPUTransferBuffer(device_, &ti);
    if (!up_ || !down_) { error_ = SDL_GetError(); releaseBuffers(); return false; }
    w_ = w;
    h_ = h;
    cell_ = C;
    dirs_ = D;
    range_ = R;
    return true;
}

void GpuLight::dispatch(SDL_GPUCommandBuffer* cmd, Kernel k, std::initializer_list<Buf> ro,
                        std::initializer_list<Buf> rw, SDL_GPUTexture* rwTex, int nx, int ny, const void* uniforms) {
    SDL_GPUStorageBufferReadWriteBinding rwb[4]{};
    Uint32 nrw = 0;
    for (Buf b : rw) rwb[nrw++].buffer = bufs_[b]; // cycle = false: kernels read what earlier ones wrote
    SDL_GPUStorageTextureReadWriteBinding tb{};
    tb.texture = rwTex;
    tb.cycle = true; // every pixel is rewritten, so a busy texture can be swapped for a fresh one
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, rwTex ? &tb : nullptr, rwTex ? 1 : 0, rwb, nrw);
    SDL_BindGPUComputePipeline(pass, pipes_[k]);
    SDL_PushGPUComputeUniformData(cmd, 0, uniforms, sizeof(Uniforms));
    SDL_GPUBuffer* rob[8]{};
    Uint32 nro = 0;
    for (Buf b : ro) rob[nro++] = bufs_[b];
    if (nro) SDL_BindGPUComputeStorageBuffers(pass, 0, rob, nro);
    SDL_DispatchGPUCompute(pass, groups(nx), groups(ny), 1);
    SDL_EndGPUComputePass(pass);
}

void GpuLight::kernels(SDL_GPUCommandBuffer* cmd, const LightParams& p, SDL_GPUTexture* target) {
    Uniforms u = makeUniforms(w_, h_, cw_, ch_, lay_, static_cast<int>(sched_.size() / 2), p);
    dispatch(cmd, kCells, {bEmit}, {bMip}, nullptr, cw_, ch_, &u);
    for (int l = 1; l < kLightLevels; ++l) {
        u.level = l;
        dispatch(cmd, kMip, {}, {bMip}, nullptr, lay_.w[l], lay_.h[l], &u);
    }
    dispatch(cmd, kSky, {bTopSky, bMip}, {bSkyTop, bSky}, nullptr, cw_, 1, &u);
    dispatch(cmd, kTrace, {bMip, bSkyTop, bSky, bDirs, bSched}, {bTrace}, nullptr, cw_, ch_, &u);
    dispatch(cmd, kTent, {bTrace}, {bLight}, nullptr, cw_, ch_, &u);
    // bloom: near level on the cells, far level on mip level 2
    auto blur = [&](Buf src, int srcOff, Buf dst, int gw, int gh, int dx, int dy) {
        u.srcW = gw; u.srcH = gh; u.dx = dx; u.dy = dy; u.srcOff = srcOff;
        dispatch(cmd, kBlur, {src}, {dst}, nullptr, gw, gh, &u);
    };
    const int bw = lay_.w[kBloomLevel], bh = lay_.h[kBloomLevel];
    blur(bMip, 0, bTmp, cw_, ch_, 1, 0);
    blur(bTmp, 0, bBloomN, cw_, ch_, 0, 1);
    blur(bMip, lay_.off[kBloomLevel], bTmpFar, bw, bh, 1, 0);
    blur(bTmpFar, 0, bBloomF, bw, bh, 0, 1);
    dispatch(cmd, kComposite, {bAlbedo, bEmit, bLight, bBloomN, bBloomF}, {}, target, w_, h_, &u);
}

bool GpuLight::record(SDL_GPUCommandBuffer* cmd, const LightInputs& in, const LightParams& p, SDL_GPUTexture* target) {
    if (!device_ || !cmd || !target || in.w <= 0 || in.h <= 0 || !ensure(in.w, in.h, p)) return false;
    auto* dst = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device_, up_, true));
    if (!dst) return false;
    struct Part { Buf buf; const void* data; };
    const Part ups[] = {{bAlbedo, in.albedo.data()}, {bEmit, in.emit.data()}, {bTopSky, in.topSky.data()},
                        {bDirs, dirTable_.data()}, {bSched, sched_.data()}};
    Uint32 off = 0;
    for (const Part& pt : ups) { std::memcpy(dst + off, pt.data, sizes_[pt.buf]); off += sizes_[pt.buf]; }
    SDL_UnmapGPUTransferBuffer(device_, up_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    off = 0;
    for (const Part& pt : ups) {
        SDL_GPUTransferBufferLocation src{up_, off};
        SDL_GPUBufferRegion reg{bufs_[pt.buf], 0, sizes_[pt.buf]};
        SDL_UploadToGPUBuffer(copy, &src, &reg, false);
        off += sizes_[pt.buf];
    }
    SDL_EndGPUCopyPass(copy);
    kernels(cmd, p, target);
    return true;
}

bool GpuLight::runSync(const LightInputs& in, const LightParams& p, SDL_GPUTexture* target, std::vector<std::uint32_t>& out) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) return false;
    if (!record(cmd, in, p, target)) { SDL_CancelGPUCommandBuffer(cmd); return false; }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion reg{};
    reg.texture = target;
    reg.w = static_cast<Uint32>(in.w);
    reg.h = static_cast<Uint32>(in.h);
    reg.d = 1;
    SDL_GPUTextureTransferInfo to{};
    to.transfer_buffer = down_;
    to.pixels_per_row = static_cast<Uint32>(in.w);
    to.rows_per_layer = static_cast<Uint32>(in.h);
    SDL_DownloadFromGPUTexture(copy, &reg, &to);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) return false;
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);
    out.resize(static_cast<std::size_t>(in.w) * in.h);
    const void* src = SDL_MapGPUTransferBuffer(device_, down_, false);
    if (!src) return false;
    std::memcpy(out.data(), src, out.size() * 4);
    SDL_UnmapGPUTransferBuffer(device_, down_);
    return true;
}

double GpuLight::benchmark(const LightParams& p, SDL_GPUTexture* target, int reps) {
    if (!device_ || w_ <= 0 || reps <= 0) return 0.0;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) return 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; ++r) kernels(cmd, p, target);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) return 0.0;
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / reps;
}

} // namespace notita
