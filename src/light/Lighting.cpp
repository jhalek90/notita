#include "Lighting.h"

#include "sim/ThreadPool.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace notita {

namespace {

constexpr float kTMin = 1.0f / 256.0f;  // a cone stops once this little gets through
constexpr int kRotOf[4] = {0, 2, 3, 1}; // 2x2 block position -> cone set (neighbours differ most)

template <class Fn>
void forRange(ThreadPool* pool, int n, const Fn& fn) {
    if (pool && n > 1) pool->parallelFor(n, fn);
    else
        for (int i = 0; i < n; ++i) fn(i);
}

struct F4 {
    float x = 0, y = 0, z = 0, w = 0;
};

F4 load4(const std::vector<float>& b, int c) { return {b[c * 4 + 0], b[c * 4 + 1], b[c * 4 + 2], b[c * 4 + 3]}; }
void store4(std::vector<float>& b, int c, F4 v) {
    b[c * 4 + 0] = v.x; b[c * 4 + 1] = v.y; b[c * 4 + 2] = v.z; b[c * 4 + 3] = v.w;
}

F4 bilerp(F4 a, F4 c, F4 d, F4 e, float fx, float fy) {
    const float w00 = (1.0f - fx) * (1.0f - fy), w10 = fx * (1.0f - fy), w01 = (1.0f - fx) * fy, w11 = fx * fy;
    return {a.x * w00 + c.x * w10 + d.x * w01 + e.x * w11, a.y * w00 + c.y * w10 + d.y * w01 + e.y * w11,
            a.z * w00 + c.z * w10 + d.z * w01 + e.z * w11, a.w * w00 + c.w * w10 + d.w * w01 + e.w * w11};
}

// bilinear over cell centres, clamped at the edges (same as the composite kernel)
F4 sample4(const std::vector<float>& b, int gw, int gh, float gx, float gy) {
    const float fx0 = std::floor(gx), fy0 = std::floor(gy);
    const int i0 = static_cast<int>(fx0), j0 = static_cast<int>(fy0);
    const int ia = std::clamp(i0, 0, gw - 1), ib = std::clamp(i0 + 1, 0, gw - 1);
    const int ja = std::clamp(j0, 0, gh - 1), jb = std::clamp(j0 + 1, 0, gh - 1);
    return bilerp(load4(b, ja * gw + ia), load4(b, ja * gw + ib), load4(b, jb * gw + ia), load4(b, jb * gw + ib), gx - fx0,
                  gy - fy0);
}

// bilinear sample of mip level L at a 16.16 position in level-0 cells; the
// fractions come from the fixed-point bits, so CPU and GPU weigh alike
F4 sampleMip(const std::vector<float>& mip, const LightLayout& lay, int L, std::int32_t px, std::int32_t py) {
    const std::int32_t uq = (px >> L) - 32768, vq = (py >> L) - 32768;
    const int i0 = uq >> 16, j0 = vq >> 16;
    const float fx = static_cast<float>(uq & 0xFFFF) * (1.0f / 65536.0f), fy = static_cast<float>(vq & 0xFFFF) * (1.0f / 65536.0f);
    const int gw = lay.w[L], gh = lay.h[L], o = lay.off[L];
    const int ia = std::clamp(i0, 0, gw - 1), ib = std::clamp(i0 + 1, 0, gw - 1);
    const int ja = std::clamp(j0, 0, gh - 1), jb = std::clamp(j0 + 1, 0, gh - 1);
    return bilerp(load4(mip, o + ja * gw + ia), load4(mip, o + ja * gw + ib), load4(mip, o + jb * gw + ia),
                  load4(mip, o + jb * gw + ib), fx, fy);
}

float shoulder(float x) {
    // linear up to the knee, then eases into 1 so bright emitters keep their hue a little longer
    const float k = 0.9f;
    if (x <= k) return x;
    return k + (1.0f - k) * (1.0f - std::exp(-(x - k) / (1.0f - k)));
}

std::uint32_t toByte(float v) {
    return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// separable blur of rgb along (dx, dy); w = 0 in the output. srcOff in float4 units.
void blur(const std::vector<float>& src, int srcOff, std::vector<float>& dst, int gw, int gh, int dx, int dy, int radius,
          const float* wt, ThreadPool* pool) {
    forRange(pool, gh, [&](int j) {
        for (int i = 0; i < gw; ++i) {
            const F4 c = load4(src, srcOff + j * gw + i);
            float r = c.x * wt[0], g = c.y * wt[0], b = c.z * wt[0];
            for (int k = 1; k <= radius; ++k) {
                const int ia = std::clamp(i - k * dx, 0, gw - 1), ja = std::clamp(j - k * dy, 0, gh - 1);
                const int ib = std::clamp(i + k * dx, 0, gw - 1), jb = std::clamp(j + k * dy, 0, gh - 1);
                const F4 a = load4(src, srcOff + ja * gw + ia), e = load4(src, srcOff + jb * gw + ib);
                r += wt[k] * (a.x + e.x);
                g += wt[k] * (a.y + e.y);
                b += wt[k] * (a.z + e.z);
            }
            store4(dst, j * gw + i, {r, g, b, 0.0f});
        }
    });
}

} // namespace

void LightInputs::resize(int width, int height) {
    w = width;
    h = height;
    albedo.assign(static_cast<std::size_t>(w) * h, 0xFF000000u);
    emit.assign(static_cast<std::size_t>(w) * h, 0u);
    topSky.assign(static_cast<std::size_t>(w), 1.0f);
}

std::uint32_t packEmit(float r, float g, float b, float opacity) {
    const float s = 1.0f / kEmitMax;
    return toByte(r * s) | (toByte(g * s) << 8) | (toByte(b * s) << 16) | (toByte(opacity) << 24);
}

LightLayout lightLayout(int cw, int ch) {
    LightLayout l;
    int w = cw, h = ch, off = 0;
    for (int L = 0; L < kLightLevels; ++L) {
        l.w[L] = w;
        l.h[L] = h;
        l.off[L] = off;
        off += w * h;
        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }
    l.total = off;
    return l;
}

std::vector<std::int32_t> lightDirections(int dirs) {
    std::vector<std::int32_t> t(static_cast<std::size_t>(kLightRotations) * dirs * 2);
    const double twoPi = 6.283185307179586;
    for (int r = 0; r < kLightRotations; ++r)
        for (int d = 0; d < dirs; ++d) {
            const double a = twoPi * (d + (r + 0.5) / kLightRotations) / dirs;
            t[(static_cast<std::size_t>(r) * dirs + d) * 2 + 0] = static_cast<std::int32_t>(std::lround(std::cos(a) * 65536.0));
            t[(static_cast<std::size_t>(r) * dirs + d) * 2 + 1] = static_cast<std::int32_t>(std::lround(std::sin(a) * 65536.0));
        }
    return t;
}

std::vector<std::int32_t> lightSchedule(int dirs, int range) {
    // a cone is 2 pi / dirs wide: sample the level whose cells are about as wide
    // as the cone at that distance, and step by that level's size
    const double theta = 6.283185307179586 / std::max(1, dirs);
    std::vector<std::int32_t> s;
    for (int d = 1; d <= range;) {
        int L = 0;
        while (L + 1 < kLightLevels && static_cast<double>(1 << (L + 1)) <= d * theta) ++L;
        s.push_back(d);
        s.push_back(L);
        d += 1 << L;
    }
    return s;
}

void gaussWeights(int radius, float sigma, float* out) {
    radius = std::clamp(radius, 0, kBlurMaxRadius);
    double sum = 0, w[kBlurMaxRadius + 1];
    for (int k = 0; k <= radius; ++k) {
        w[k] = std::exp(-0.5 * k * k / std::max(1e-3, static_cast<double>(sigma) * sigma));
        sum += k == 0 ? w[k] : 2.0 * w[k];
    }
    for (int k = 0; k <= kBlurMaxRadius; ++k) out[k] = k <= radius ? static_cast<float>(w[k] / sum) : 0.0f;
}

void CpuLight::run(const LightInputs& in, const LightParams& p, std::vector<std::uint32_t>& out, ThreadPool* pool) {
    const auto t0 = std::chrono::steady_clock::now();
    const int W = in.w, H = in.h, C = std::max(1, p.cellPx);
    const int cw = (W + C - 1) / C, ch = (H + C - 1) / C;
    const int D = std::max(1, p.dirs), R = std::max(1, p.range);
    out.assign(static_cast<std::size_t>(W) * H, 0xFF000000u);
    if (W <= 0 || H <= 0) return;
    const std::size_t nc = static_cast<std::size_t>(cw) * ch;
    const LightLayout lay = lightLayout(cw, ch);
    const int bw = lay.w[kBloomLevel], bh = lay.h[kBloomLevel];
    f_.cw = cw; f_.ch = ch; f_.mipLayout = lay;
    f_.mip.assign(static_cast<std::size_t>(lay.total) * 4, 0.0f);
    f_.skyTop.assign(static_cast<std::size_t>(cw), 0.0f);
    f_.sky.assign(nc, 0.0f);
    f_.trace.assign(nc * 4, 0.0f);
    f_.light.assign(nc * 4, 0.0f);
    f_.bloomNear.assign(nc * 4, 0.0f);
    f_.bloomFar.assign(static_cast<std::size_t>(bw) * bh * 4, 0.0f);
    tmp_.assign(nc * 4, 0.0f);
    tmpFar_.assign(f_.bloomFar.size(), 0.0f);
    if (dirTableN_ != D) { dirTable_ = lightDirections(D); dirTableN_ = D; }
    if (schedDirs_ != D || schedRange_ != R) { sched_ = lightSchedule(D, R); schedDirs_ = D; schedRange_ = R; }
    const int nSched = static_cast<int>(sched_.size() / 2);
    std::vector<float>& mip = f_.mip;

    // 1. cells (mip level 0): integer sums of the packed bytes, then one scale
    forRange(pool, ch, [&](int j) {
        for (int i = 0; i < cw; ++i) {
            std::uint32_t er = 0, eg = 0, eb = 0, ea = 0, n = 0;
            for (int y = j * C; y < std::min(j * C + C, H); ++y)
                for (int x = i * C; x < std::min(i * C + C, W); ++x) {
                    const std::uint32_t e = in.emit[static_cast<std::size_t>(y) * W + x];
                    er += e & 0xFF; eg += (e >> 8) & 0xFF; eb += (e >> 16) & 0xFF; ea += e >> 24; ++n;
                }
            const float fn = static_cast<float>(n);
            const float k = kEmitMax / 255.0f;
            const float a = static_cast<float>(ea) / (255.0f * fn);
            float t = 1.0f;
            for (int s = 0; s < C; ++s) t *= 1.0f - a; // a is per pixel, a step crosses C of them
            store4(mip, j * cw + i, {static_cast<float>(er) * k / fn, static_cast<float>(eg) * k / fn,
                                     static_cast<float>(eb) * k / fn, t});
        }
    });

    // 2. mip levels: emission averaged, transmittance halfway between the mean and the minimum
    for (int L = 1; L < kLightLevels; ++L) {
        const int pw = lay.w[L - 1], ph = lay.h[L - 1], po = lay.off[L - 1];
        const int lw = lay.w[L], o = lay.off[L];
        forRange(pool, lay.h[L], [&](int j) {
            for (int i = 0; i < lw; ++i) {
                float r = 0, g = 0, b = 0, ts = 0, tmin = 1.0f;
                int n = 0;
                for (int y = 2 * j; y < std::min(2 * j + 2, ph); ++y)
                    for (int x = 2 * i; x < std::min(2 * i + 2, pw); ++x) {
                        const F4 v = load4(mip, po + y * pw + x);
                        r += v.x; g += v.y; b += v.z; ts += v.w; tmin = std::min(tmin, v.w); ++n;
                    }
                const float fn = static_cast<float>(n);
                store4(mip, o + j * lw + i, {r / fn, g / fn, b / fn, 0.5f * (ts / fn + tmin)});
            }
        });
    }

    // 3. sky: straight down each cell column from what reaches the view's top
    forRange(pool, cw, [&](int i) {
        float top = 0.0f;
        int n = 0;
        for (int x = i * C; x < std::min(i * C + C, W); ++x) { top += in.topSky[static_cast<std::size_t>(x)]; ++n; }
        top /= static_cast<float>(n);
        f_.skyTop[static_cast<std::size_t>(i)] = top;
        float T = top;
        for (int j = 0; j < ch; ++j) {
            f_.sky[static_cast<std::size_t>(j) * cw + i] = T;
            T *= mip[(static_cast<std::size_t>(j) * cw + i) * 4 + 3];
        }
    });

    // 4. trace (cones)
    const float invRange = 1.0f / static_cast<float>(R + 1);
    const float invDirs = 1.0f / static_cast<float>(D);
    forRange(pool, ch, [&](int j) {
        for (int i = 0; i < cw; ++i) {
            const int rot = kRotOf[(i & 1) + 2 * (j & 1)];
            const std::int32_t px0 = (i << 16) + 32768, py0 = (j << 16) + 32768;
            float lr = 0, lg = 0, lb = 0, sky = 0;
            for (int d = 0; d < D; ++d) {
                const std::int32_t dx = dirTable_[(static_cast<std::size_t>(rot) * D + d) * 2];
                const std::int32_t dy = dirTable_[(static_cast<std::size_t>(rot) * D + d) * 2 + 1];
                float T = 1.0f, r = 0, g = 0, b = 0;
                int last = j * cw + i;
                for (int k = 0; k < nSched; ++k) {
                    const int dist = sched_[static_cast<std::size_t>(k) * 2], L = sched_[static_cast<std::size_t>(k) * 2 + 1];
                    const std::int32_t px = px0 + dx * dist, py = py0 + dy * dist;
                    if ((py >> 16) < 0) { // out through the top: the open sky (if the column has it)
                        sky += T * f_.skyTop[static_cast<std::size_t>(std::clamp(px >> 16, 0, cw - 1))];
                        T = 0.0f;
                        break;
                    }
                    const F4 v = sampleMip(mip, lay, L, px, py);
                    // crossing 2^L cells of this level lets t^(2^L) through, and the
                    // emission adds up along the way as a geometric series
                    float tn = v.w;
                    for (int l = 0; l < L; ++l) tn *= tn;
                    const float span = v.w > 0.9999f ? static_cast<float>(1 << L) : (1.0f - tn) / (1.0f - v.w);
                    const float f = static_cast<float>(dist) * invRange;
                    const float tw = T * (1.0f - f * f) * span; // fades out towards the end of the range
                    r += tw * v.x;
                    g += tw * v.y;
                    b += tw * v.z;
                    T *= tn;
                    last = std::min(py >> 16, ch - 1) * cw + std::clamp(px >> 16, 0, cw - 1);
                    if (T < kTMin) { T = 0.0f; break; }
                }
                sky += T * f_.sky[static_cast<std::size_t>(last)]; // ran out of range: the far field
                lr += r; lg += g; lb += b;
            }
            store4(f_.trace, j * cw + i, {lr * invDirs * p.gain, lg * invDirs * p.gain, lb * invDirs * p.gain, sky * invDirs});
        }
    });

    // 5. tent: blends the four rotated cone sets
    forRange(pool, ch, [&](int j) {
        for (int i = 0; i < cw; ++i) {
            F4 s;
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) {
                    const float wgt = static_cast<float>((2 - std::abs(di)) * (2 - std::abs(dj)));
                    const F4 v = load4(f_.trace, std::clamp(j + dj, 0, ch - 1) * cw + std::clamp(i + di, 0, cw - 1));
                    s.x += wgt * v.x; s.y += wgt * v.y; s.z += wgt * v.z; s.w += wgt * v.w;
                }
            store4(f_.light, j * cw + i, {s.x * 0.0625f, s.y * 0.0625f, s.z * 0.0625f, s.w * 0.0625f});
        }
    });

    // 6. bloom: near level on the cells, far level on mip level 2
    float wt[kBlurMaxRadius + 1];
    gaussWeights(p.bloomRadius, p.bloomSigma, wt);
    const int BR = std::clamp(p.bloomRadius, 0, kBlurMaxRadius);
    blur(mip, 0, tmp_, cw, ch, 1, 0, BR, wt, pool);
    blur(tmp_, 0, f_.bloomNear, cw, ch, 0, 1, BR, wt, pool);
    blur(mip, lay.off[kBloomLevel], tmpFar_, bw, bh, 1, 0, BR, wt, pool);
    blur(tmpFar_, 0, f_.bloomFar, bw, bh, 0, 1, BR, wt, pool);

    // 7. composite
    const float invC = 1.0f / static_cast<float>(C), invCF = 1.0f / static_cast<float>(C << kBloomLevel);
    const float ek = kEmitMax / 255.0f;
    const float bn = p.bloom * p.bloomNear, bf = p.bloom * p.bloomFar;
    forRange(pool, H, [&](int y) {
        const float gy = (static_cast<float>(y) + 0.5f) * invC - 0.5f;
        const float fy = (static_cast<float>(y) + 0.5f) * invCF - 0.5f;
        for (int x = 0; x < W; ++x) {
            const std::size_t pi = static_cast<std::size_t>(y) * W + x;
            const float gx = (static_cast<float>(x) + 0.5f) * invC - 0.5f;
            const float fx = (static_cast<float>(x) + 0.5f) * invCF - 0.5f;
            const F4 L = sample4(f_.light, cw, ch, gx, gy);
            const F4 Bn = sample4(f_.bloomNear, cw, ch, gx, gy);
            const F4 Bf = sample4(f_.bloomFar, bw, bh, fx, fy);
            const float sk = p.sky * std::min(1.0f, L.w * p.skyGain);
            const float lr = p.ambient + sk * p.skyR + L.x, lg = p.ambient + sk * p.skyG + L.y, lb = p.ambient + sk * p.skyB + L.z;
            const std::uint32_t a = in.albedo[pi], e = in.emit[pi];
            const float ar = static_cast<float>(a & 0xFF) / 255.0f, ag = static_cast<float>((a >> 8) & 0xFF) / 255.0f,
                        ab = static_cast<float>((a >> 16) & 0xFF) / 255.0f;
            const float hz = p.haze * (1.0f - static_cast<float>(e >> 24) / 255.0f);
            const float orr = ar * ar * lr + hz * L.x + static_cast<float>(e & 0xFF) * ek + bn * Bn.x + bf * Bf.x;
            const float og = ag * ag * lg + hz * L.y + static_cast<float>((e >> 8) & 0xFF) * ek + bn * Bn.y + bf * Bf.y;
            const float ob = ab * ab * lb + hz * L.z + static_cast<float>((e >> 16) & 0xFF) * ek + bn * Bn.z + bf * Bf.z;
            out[pi] = toByte(std::sqrt(shoulder(orr))) | (toByte(std::sqrt(shoulder(og))) << 8) |
                      (toByte(std::sqrt(shoulder(ob))) << 16) | 0xFF000000u;
        }
    });
    lastMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace notita
