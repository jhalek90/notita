#include "GpuLiquid.h"

#include <cstring>

namespace notita {

namespace {

// Matches cbuffer Params in liquid.hlsl.
struct Uniforms {
    int w, h, s, flags;
    float time, pad0, pad1, pad2;
    float shine[kLiquidLooks][4];
    float foam[kLiquidLooks][4];
    float misc[kLiquidLooks][4];
};
static_assert(sizeof(Uniforms) == 32 + 3 * kLiquidLooks * 16, "keep Uniforms in step with liquid.hlsl");

} // namespace

bool GpuLiquid::init(SDL_GPUDevice* device, const std::string& shaderDir) {
    device_ = device;
    if (!(SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_DXIL)) {
        error_ = "the GPU device does not take DXIL shaders";
        return false;
    }
    if (!SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE)) {
        error_ = "RGBA8 storage textures are not supported";
        return false;
    }
    const std::string path = shaderDir + "liquid.dxil";
    size_t size = 0;
    void* code = SDL_LoadFile(path.c_str(), &size);
    if (!code) {
        error_ = "missing shader " + path;
        return false;
    }
    SDL_GPUComputePipelineCreateInfo ci{};
    ci.code = static_cast<const Uint8*>(code);
    ci.code_size = size;
    ci.entrypoint = "main";
    ci.format = SDL_GPU_SHADERFORMAT_DXIL;
    ci.num_readonly_storage_textures = 1;
    ci.num_readonly_storage_buffers = 1;
    ci.num_readwrite_storage_textures = 1;
    ci.num_uniform_buffers = 1;
    ci.threadcount_x = 8;
    ci.threadcount_y = 8;
    ci.threadcount_z = 1;
    pipe_ = SDL_CreateGPUComputePipeline(device, &ci);
    SDL_free(code);
    if (!pipe_) {
        error_ = std::string("liquid pipeline: ") + SDL_GetError();
        return false;
    }
    return true;
}

void GpuLiquid::release() {
    if (!device_) return;
    if (out_) SDL_ReleaseGPUTexture(device_, out_);
    if (info_) SDL_ReleaseGPUBuffer(device_, info_);
    if (up_) SDL_ReleaseGPUTransferBuffer(device_, up_);
    if (down_) SDL_ReleaseGPUTransferBuffer(device_, down_);
    out_ = nullptr;
    info_ = nullptr;
    up_ = down_ = nullptr;
    w_ = h_ = s_ = ow_ = oh_ = 0;
}

void GpuLiquid::shutdown() {
    if (!device_) return;
    release();
    if (pipe_) SDL_ReleaseGPUComputePipeline(device_, pipe_);
    pipe_ = nullptr;
    device_ = nullptr;
}

bool GpuLiquid::ensure(int w, int h, int scale) {
    if (w == w_ && h == h_ && scale == s_ && out_) return true;
    release();
    ow_ = w * scale;
    oh_ = h * scale;
    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    tci.width = static_cast<Uint32>(ow_);
    tci.height = static_cast<Uint32>(oh_);
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    out_ = SDL_CreateGPUTexture(device_, &tci);
    SDL_GPUBufferCreateInfo bi{};
    bi.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    bi.size = static_cast<Uint32>(w) * h * 4u;
    info_ = SDL_CreateGPUBuffer(device_, &bi);
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = bi.size;
    up_ = SDL_CreateGPUTransferBuffer(device_, &ti);
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = static_cast<Uint32>(ow_) * oh_ * 4u;
    down_ = SDL_CreateGPUTransferBuffer(device_, &ti);
    if (!out_ || !info_ || !up_ || !down_) {
        error_ = SDL_GetError();
        release();
        return false;
    }
    w_ = w;
    h_ = h;
    s_ = scale;
    return true;
}

bool GpuLiquid::record(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* scene, const std::vector<std::uint32_t>& info, int w, int h,
                       int scale, float time, bool wobble, const std::array<LiquidLook, kLiquidLooks>& looks) {
    if (!device_ || !pipe_ || !cmd || !scene || w <= 0 || h <= 0 || info.size() < static_cast<std::size_t>(w) * h) return false;
    if (!ensure(w, h, scale)) return false;
    if (void* dst = SDL_MapGPUTransferBuffer(device_, up_, true)) {
        std::memcpy(dst, info.data(), static_cast<std::size_t>(w) * h * 4u);
        SDL_UnmapGPUTransferBuffer(device_, up_);
    } else {
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src{up_, 0};
    SDL_GPUBufferRegion reg{info_, 0, static_cast<Uint32>(w) * h * 4u};
    SDL_UploadToGPUBuffer(copy, &src, &reg, true);
    SDL_EndGPUCopyPass(copy);

    Uniforms u{};
    u.w = w;
    u.h = h;
    u.s = scale;
    u.flags = wobble ? 1 : 0;
    u.time = time;
    for (int k = 0; k < kLiquidLooks; ++k) {
        const LiquidLook& l = looks[static_cast<std::size_t>(k)];
        std::memcpy(u.shine[k], l.shine, sizeof(l.shine));
        std::memcpy(u.foam[k], l.foam, sizeof(l.foam));
        u.misc[k][0] = l.wobble;
        u.misc[k][1] = l.shimmer;
        u.misc[k][2] = l.glint;
        u.misc[k][3] = 0.0f;
    }

    SDL_GPUStorageTextureReadWriteBinding tb{};
    tb.texture = out_;
    tb.cycle = true; // every pixel is rewritten
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &tb, 1, nullptr, 0);
    SDL_BindGPUComputePipeline(pass, pipe_);
    SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
    SDL_BindGPUComputeStorageTextures(pass, 0, &scene, 1);
    SDL_BindGPUComputeStorageBuffers(pass, 0, &info_, 1);
    SDL_DispatchGPUCompute(pass, static_cast<Uint32>((ow_ + 7) / 8), static_cast<Uint32>((oh_ + 7) / 8), 1);
    SDL_EndGPUComputePass(pass);
    return true;
}

bool GpuLiquid::readBack(std::vector<std::uint32_t>& px) {
    if (!device_ || !out_) return false;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) return false;
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion reg{};
    reg.texture = out_;
    reg.w = static_cast<Uint32>(ow_);
    reg.h = static_cast<Uint32>(oh_);
    reg.d = 1;
    SDL_GPUTextureTransferInfo to{};
    to.transfer_buffer = down_;
    to.pixels_per_row = static_cast<Uint32>(ow_);
    to.rows_per_layer = static_cast<Uint32>(oh_);
    SDL_DownloadFromGPUTexture(copy, &reg, &to);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) return false;
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);
    px.resize(static_cast<std::size_t>(ow_) * oh_);
    const void* src = SDL_MapGPUTransferBuffer(device_, down_, false);
    if (!src) return false;
    std::memcpy(px.data(), src, px.size() * 4);
    SDL_UnmapGPUTransferBuffer(device_, down_);
    return true;
}

} // namespace notita
