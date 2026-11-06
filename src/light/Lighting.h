#pragma once
// 2D light transport for the view (phase 6), the SDL-free reference of the
// GPU kernels in src/app/shaders/light.hlsl (same kernels, same order, same
// integer ray stepping; keep the two in sync and run notita --light-selftest).
//
// Inputs are per view pixel, packed to 8 bits so CPU and GPU see the same
// bytes: the albedo (the colour buffer the app already fills), the emission
// (rgb, scaled by kEmitMax) with the opacity in alpha, and per pixel column
// how much sky reaches the top row of the view.
//
// Kernels:
//   1. cells     average emission/opacity into light cells (cellPx x cellPx) = mip level 0
//   2. mip       levels 1..4: emission averaged, transmittance between the mean
//                and the minimum (so thin walls still block from far away)
//   3. sky       per cell column: sky transmittance straight down from the top
//   4. trace     2D cone tracing: per cell `dirs` cones; each step samples the
//                mip level as wide as the cone there (bilinear, fixed-point
//                fractions) and moves by that level's size, so far light is
//                smooth and shadows soften with distance. Cones that leave
//                the top see the sky; the ones that run out of range take
//                the vertical sky of the cell they end in (open air far away
//                is lit by the sky, a cave is not). 4 rotated cone sets in a
//                2x2 pattern...
//   5. tent      ...that a 3x3 tent blends into 4x as many directions
//   6. bloom     emission blurred on level 0 and on level 2 (4x coarser)
//   7. composite out = albedo * (ambient + sky + light) + haze * light * (1 - opacity)
//                + emission + bloom, in a
//                gamma-2 linear space with a soft shoulder above 0.9
// Everything inside a kernel is per-cell independent, so the CPU version is
// the same at any thread count.
#include <cstdint>
#include <vector>

namespace notita {

class ThreadPool;

constexpr float kEmitMax = 4.0f;   // packed emission byte 255 = this much radiance
constexpr int kLightRotations = 4; // cone sets, one per cell of a 2x2 block
constexpr int kLightLevels = 5;    // mip levels 0..4 (1..16 cells)
constexpr int kBloomLevel = 2;     // the far bloom blurs this mip level
constexpr int kBlurMaxRadius = 8;

struct LightParams {
    int   cellPx = 2;      // pixels per light cell (2 on the GPU, 4 for the CPU fallback)
    int   dirs = 32;       // cones per cell (x4 rotations after the tent)
    int   range = 96;      // cone length in cells
    float gain = 1.6f;     // emitted light reaching other pixels (the emitters themselves show it 1:1)
    float ambient = 0.08f; // everywhere, so caves are dark but not black
    float sky = 1.0f;      // sky light strength
    float skyGain = 2.0f;  // a floor sees half the sky; this brings it back near 1 (clamped)
    float skyR = 0.92f, skyG = 0.96f, skyB = 1.0f;
    float haze = 0.06f;    // air shows this much of the traced light, so light and shadow read in open caves
    float bloom = 0.4f;    // 0 = off
    float bloomNear = 0.7f, bloomFar = 0.5f; // weights of the two bloom levels
    int   bloomRadius = 6; // taps each side, both levels
    float bloomSigma = 3.0f;
};

struct LightInputs {
    int w = 0, h = 0;
    std::vector<std::uint32_t> albedo; // w*h RGBA8 (memory order r,g,b,a)
    std::vector<std::uint32_t> emit;   // w*h: rgb = emission / kEmitMax * 255, a = opacity * 255
    std::vector<float> topSky;         // w: 0..1 sky reaching the top row of each column
    void resize(int width, int height);
};

std::uint32_t packEmit(float r, float g, float b, float opacity);

// Mip chain of the light cells, all levels in one buffer of float4.
struct LightLayout {
    int w[kLightLevels]{}, h[kLightLevels]{}, off[kLightLevels]{}; // off in float4 units
    int total = 0;
};
LightLayout lightLayout(int cw, int ch);

// Cone directions for (rotation, dir) as 16.16 fixed-point steps of one cell,
// laid out [rotation * dirs + dir] as (dx, dy) pairs. Integer stepping keeps
// the cells a cone visits bit-identical between the CPU and the GPU.
std::vector<std::int32_t> lightDirections(int dirs);
// The cone's steps as (distance in cells, mip level) pairs, out to range.
std::vector<std::int32_t> lightSchedule(int dirs, int range);
// Normalised gaussian taps 0..radius (tap 0 is the centre).
void gaussWeights(int radius, float sigma, float* out /* kBlurMaxRadius + 1 */);

struct LightFields {
    int cw = 0, ch = 0;             // light cells
    LightLayout mipLayout;
    std::vector<float> mip;         // layout.total*4: emission rgb, transmittance per cell step
    std::vector<float> skyTop;      // cw
    std::vector<float> sky;         // cw*ch
    std::vector<float> trace;       // cw*ch*4: light rgb, sky visibility
    std::vector<float> light;       // cw*ch*4: after the tent
    std::vector<float> bloomNear;   // cw*ch*4
    std::vector<float> bloomFar;    // level-2 size *4
};

class CpuLight {
public:
    // Runs all kernels; out gets w*h RGBA8. pool may be null (single thread).
    void run(const LightInputs& in, const LightParams& p, std::vector<std::uint32_t>& out, ThreadPool* pool = nullptr);
    const LightFields& fields() const { return f_; }
    double lastMs() const { return lastMs_; }

private:
    LightFields f_;
    std::vector<float> tmp_, tmpFar_;
    std::vector<std::int32_t> dirTable_, sched_;
    int dirTableN_ = -1, schedDirs_ = -1, schedRange_ = -1;
    double lastMs_ = 0.0;
};

} // namespace notita
