#include "GpuFlame.h"

#include <cstring>

namespace notita {

namespace {

// Matches cbuffer Params in flame.hlsl.
struct Uniforms {
    int w, h, s, flags;
    int sw, sh, cw, ch;
    float offX, offY, time, gain;
    float smoke, flicker, minTemp, pad;
    float orgX, orgY, pad1, pad2;
    float tint[kFlameTints][4];
};
static_assert(sizeof(Uniforms) == 80 + kFlameTints * 16, "keep Uniforms in step with flame.hlsl");
static_assert(sizeof(FlameCell) == 16, "FlameCell is a float4 on the GPU");

} // namespace

bool GpuFlame::init(SDL_GPUDevice* device, const std::string& shaderDir) {
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
    const std::string path = shaderDir + "flame.dxil";
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
        error_ = std::string("flame pipeline: ") + SDL_GetError();
        return false;
    }
    return true;
}

void GpuFlame::release() {
    if (!device_) return;
    if (out_) SDL_ReleaseGPUTexture(device_, out_);
    if (cells_) SDL_ReleaseGPUBuffer(device_, cells_);
    if (up_) SDL_ReleaseGPUTransferBuffer(device_, up_);
    if (down_) SDL_ReleaseGPUTransferBuffer(device_, down_);
    out_ = nullptr;
    cells_ = nullptr;
    up_ = down_ = nullptr;
    ow_ = oh_ = ncells_ = 0;
}

void GpuFlame::shutdown() {
    if (!device_) return;
    release();
    if (pipe_) SDL_ReleaseGPUComputePipeline(device_, pipe_);
    pipe_ = nullptr;
    device_ = nullptr;
}

bool GpuFlame::ensure(int ow, int oh, int cells) {
    if (ow == ow_ && oh == oh_ && cells == ncells_ && out_) return true;
    release();
    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    tci.width = static_cast<Uint32>(ow);
    tci.height = static_cast<Uint32>(oh);
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    out_ = SDL_CreateGPUTexture(device_, &tci);
    SDL_GPUBufferCreateInfo bi{};
    bi.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    bi.size = static_cast<Uint32>(cells) * sizeof(FlameCell);
    cells_ = SDL_CreateGPUBuffer(device_, &bi);
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = bi.size;
    up_ = SDL_CreateGPUTransferBuffer(device_, &ti);
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = static_cast<Uint32>(ow) * oh * 4u;
    down_ = SDL_CreateGPUTransferBuffer(device_, &ti);
    if (!out_ || !cells_ || !up_ || !down_) {
        error_ = SDL_GetError();
        release();
        return false;
    }
    ow_ = ow;
    oh_ = oh;
    ncells_ = cells;
    return true;
}

bool GpuFlame::record(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* scene, int sw, int sh, const std::vector<FlameCell>& cells,
                      int cw, int ch, float offX, float offY, float orgX, float orgY, int w, int h, int scale, float time, const FlameLook& look,
                      const std::array<std::array<float, 4>, kFlameTints>& tints) {
    if (!device_ || !pipe_ || !cmd || !scene || w <= 0 || h <= 0 || cw <= 0 || ch <= 0 ||
        cells.size() < static_cast<std::size_t>(cw) * ch)
        return false;
    if (!ensure(w * scale, h * scale, cw * ch)) return false;
    const std::size_t bytes = static_cast<std::size_t>(cw) * ch * sizeof(FlameCell);
    if (void* dst = SDL_MapGPUTransferBuffer(device_, up_, true)) {
        std::memcpy(dst, cells.data(), bytes);
        SDL_UnmapGPUTransferBuffer(device_, up_);
    } else {
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src{up_, 0};
    SDL_GPUBufferRegion reg{cells_, 0, static_cast<Uint32>(bytes)};
    SDL_UploadToGPUBuffer(copy, &src, &reg, true);
    SDL_EndGPUCopyPass(copy);

    Uniforms u{};
    u.w = w;
    u.h = h;
    u.s = scale;
    u.sw = sw;
    u.sh = sh;
    u.cw = cw;
    u.ch = ch;
    u.offX = offX;
    u.offY = offY;
    u.orgX = orgX;
    u.orgY = orgY;
    u.time = time;
    u.gain = look.gain;
    u.smoke = look.smoke;
    u.flicker = look.flicker;
    u.minTemp = look.minTemp;
    for (int k = 0; k < kFlameTints; ++k) std::memcpy(u.tint[k], tints[static_cast<std::size_t>(k)].data(), sizeof(u.tint[k]));

    SDL_GPUStorageTextureReadWriteBinding tb{};
    tb.texture = out_;
    tb.cycle = true; // every pixel is rewritten
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &tb, 1, nullptr, 0);
    SDL_BindGPUComputePipeline(pass, pipe_);
    SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
    SDL_BindGPUComputeStorageTextures(pass, 0, &scene, 1);
    SDL_BindGPUComputeStorageBuffers(pass, 0, &cells_, 1);
    SDL_DispatchGPUCompute(pass, static_cast<Uint32>((ow_ + 7) / 8), static_cast<Uint32>((oh_ + 7) / 8), 1);
    SDL_EndGPUComputePass(pass);
    return true;
}

bool GpuFlame::readBack(std::vector<std::uint32_t>& px) {
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
