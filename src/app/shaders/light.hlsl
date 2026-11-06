// View lighting kernels for the SDL3 GPU API (compiled once per kernel with
// -D K_<NAME>, entry point "main"). They mirror CpuLight (src/light/Lighting.cpp)
// step by step, including the 16.16 integer cone stepping and fixed-point
// bilinear fractions; keep the two in sync and run notita --light-selftest.
// Bindings follow SDL's DXIL rules: read-only storage buffers in (t[n], space0),
// read-write storage textures then buffers in (u[n], space1), uniforms in (b0, space2).
// Buffers are scalar (float/uint/int) because SDL makes raw views; float4 data
// is indexed c * 4 + k by hand.

cbuffer Params : register(b0, space2)
{
    int W; int H; int CW; int CH;          // view pixels, light cells
    int C; int D; int NSched; int Level;   // cell px, cones per cell, schedule length, mip level being built
    int SrcW; int SrcH; int DX; int DY;    // blur source grid and direction
    int Radius; int SrcOff; int Range; int Pad0;
    int4 MipW[2]; int4 MipH[2]; int4 MipOff[2]; // levels 0..4
    float Gain; float Ambient; float Sky; float SkyGain;
    float SkyR; float SkyG; float SkyB; float BloomN;
    float BloomF; float InvRange; float InvDirs; float InvC;
    float InvCF; float EmitK; float Haze; float Pad1;
    float4 Wt[3];                          // gaussian taps 0..8
};

static const float TMin = 1.0 / 256.0;
static const int BloomLevel = 2;

int mipW(int l) { return MipW[l >> 2][l & 3]; }
int mipH(int l) { return MipH[l >> 2][l & 3]; }
int mipOff(int l) { return MipOff[l >> 2][l & 3]; }

#define LOAD4(b, c) float4(b[(c) * 4 + 0], b[(c) * 4 + 1], b[(c) * 4 + 2], b[(c) * 4 + 3])
#define STORE4(buf, c, v)          \
    {                              \
        buf[(c) * 4 + 0] = (v).x;  \
        buf[(c) * 4 + 1] = (v).y;  \
        buf[(c) * 4 + 2] = (v).z;  \
        buf[(c) * 4 + 3] = (v).w;  \
    }

float4 bilerp(float4 a, float4 c, float4 d, float4 e, float fx, float fy)
{
    float w00 = (1.0 - fx) * (1.0 - fy), w10 = fx * (1.0 - fy), w01 = (1.0 - fx) * fy, w11 = fx * fy;
    return float4(a.x * w00 + c.x * w10 + d.x * w01 + e.x * w11, a.y * w00 + c.y * w10 + d.y * w01 + e.y * w11,
                  a.z * w00 + c.z * w10 + d.z * w01 + e.z * w11, a.w * w00 + c.w * w10 + d.w * w01 + e.w * w11);
}

// ---------------------------------------------------------- 1. cells (mip 0)
#ifdef K_CELLS
StructuredBuffer<uint> Emit : register(t0, space0);
RWStructuredBuffer<float> Mip : register(u0, space1);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int i = (int)id.x, j = (int)id.y;
    if (i >= CW || j >= CH) return;
    uint er = 0, eg = 0, eb = 0, ea = 0, n = 0;
    for (int y = j * C; y < min(j * C + C, H); ++y)
        for (int x = i * C; x < min(i * C + C, W); ++x) {
            uint e = Emit[y * W + x];
            er += e & 0xFF; eg += (e >> 8) & 0xFF; eb += (e >> 16) & 0xFF; ea += e >> 24; ++n;
        }
    float fn = (float)n;
    float k = EmitK;
    float a = (float)ea / (255.0 * fn);
    float t = 1.0;
    for (int s = 0; s < C; ++s) t *= 1.0 - a;
    float4 v = float4((float)er * k / fn, (float)eg * k / fn, (float)eb * k / fn, t);
    STORE4(Mip, j * CW + i, v);
}
#endif

// ------------------------------------------------------- 2. one mip level
#ifdef K_MIP
RWStructuredBuffer<float> Mip : register(u0, space1);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int i = (int)id.x, j = (int)id.y;
    int L = Level;
    int lw = mipW(L);
    if (i >= lw || j >= mipH(L)) return;
    int pw = mipW(L - 1), ph = mipH(L - 1), po = mipOff(L - 1);
    float r = 0, g = 0, b = 0, ts = 0, tmin = 1.0;
    int n = 0;
    for (int y = 2 * j; y < min(2 * j + 2, ph); ++y)
        for (int x = 2 * i; x < min(2 * i + 2, pw); ++x) {
            float4 v = LOAD4(Mip, po + y * pw + x);
            r += v.x; g += v.y; b += v.z; ts += v.w; tmin = min(tmin, v.w); ++n;
        }
    float fn = (float)n;
    float4 o = float4(r / fn, g / fn, b / fn, 0.5 * (ts / fn + tmin));
    STORE4(Mip, mipOff(L) + j * lw + i, o);
}
#endif

// ------------------------------------------------------------------ 3. sky
#ifdef K_SKY
StructuredBuffer<float> TopSky : register(t0, space0);
StructuredBuffer<float> Mip : register(t1, space0);
RWStructuredBuffer<float> SkyTop : register(u0, space1);
RWStructuredBuffer<float> SkyBuf : register(u1, space1);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int i = (int)id.x;
    if (i >= CW || id.y != 0) return;
    float top = 0.0;
    int n = 0;
    for (int x = i * C; x < min(i * C + C, W); ++x) { top += TopSky[x]; ++n; }
    top /= (float)n;
    SkyTop[i] = top;
    float T = top;
    for (int j = 0; j < CH; ++j) {
        SkyBuf[j * CW + i] = T;
        T *= Mip[(j * CW + i) * 4 + 3];
    }
}
#endif

// ---------------------------------------------------------- 4. trace (cones)
#ifdef K_TRACE
StructuredBuffer<float> Mip : register(t0, space0);
StructuredBuffer<float> SkyTop : register(t1, space0);
StructuredBuffer<float> SkyBuf : register(t2, space0);
StructuredBuffer<int> Dirs : register(t3, space0);
StructuredBuffer<int> Sched : register(t4, space0);
RWStructuredBuffer<float> Trace : register(u0, space1);

float4 sampleMip(int L, int px, int py)
{
    int uq = (px >> L) - 32768, vq = (py >> L) - 32768;
    int i0 = uq >> 16, j0 = vq >> 16;
    float fx = (float)(uq & 0xFFFF) * (1.0 / 65536.0), fy = (float)(vq & 0xFFFF) * (1.0 / 65536.0);
    int gw = mipW(L), gh = mipH(L), o = mipOff(L);
    int ia = clamp(i0, 0, gw - 1), ib = clamp(i0 + 1, 0, gw - 1);
    int ja = clamp(j0, 0, gh - 1), jb = clamp(j0 + 1, 0, gh - 1);
    return bilerp(LOAD4(Mip, o + ja * gw + ia), LOAD4(Mip, o + ja * gw + ib), LOAD4(Mip, o + jb * gw + ia),
                  LOAD4(Mip, o + jb * gw + ib), fx, fy);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int i = (int)id.x, j = (int)id.y;
    if (i >= CW || j >= CH) return;
    static const int rotOf[4] = {0, 2, 3, 1};
    int rot = rotOf[(i & 1) + 2 * (j & 1)];
    int px0 = (i << 16) + 32768, py0 = (j << 16) + 32768;
    float lr = 0, lg = 0, lb = 0, sky = 0;
    for (int d = 0; d < D; ++d) {
        int dx = Dirs[(rot * D + d) * 2];
        int dy = Dirs[(rot * D + d) * 2 + 1];
        float T = 1.0, r = 0, g = 0, b = 0;
        int last = j * CW + i;
        for (int k = 0; k < NSched; ++k) {
            int dist = Sched[k * 2], L = Sched[k * 2 + 1];
            int px = px0 + dx * dist, py = py0 + dy * dist;
            if ((py >> 16) < 0) {
                sky += T * SkyTop[clamp(px >> 16, 0, CW - 1)];
                T = 0.0;
                break;
            }
            float4 v = sampleMip(L, px, py);
            float tn = v.w;
            for (int l = 0; l < L; ++l) tn *= tn;
            float span = v.w > 0.9999 ? (float)(1 << L) : (1.0 - tn) / (1.0 - v.w);
            float f = (float)dist * InvRange;
            float tw = T * (1.0 - f * f) * span;
            r += tw * v.x;
            g += tw * v.y;
            b += tw * v.z;
            T *= tn;
            last = min(py >> 16, CH - 1) * CW + clamp(px >> 16, 0, CW - 1);
            if (T < TMin) { T = 0.0; break; }
        }
        sky += T * SkyBuf[last];
        lr += r; lg += g; lb += b;
    }
    float4 v = float4(lr * InvDirs * Gain, lg * InvDirs * Gain, lb * InvDirs * Gain, sky * InvDirs);
    STORE4(Trace, j * CW + i, v);
}
#endif

// ----------------------------------------------------------------- 5. tent
#ifdef K_TENT
StructuredBuffer<float> Trace : register(t0, space0);
RWStructuredBuffer<float> Light : register(u0, space1);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int i = (int)id.x, j = (int)id.y;
    if (i >= CW || j >= CH) return;
    float4 s = float4(0, 0, 0, 0);
    for (int dj = -1; dj <= 1; ++dj)
        for (int di = -1; di <= 1; ++di) {
            float wgt = (float)((2 - abs(di)) * (2 - abs(dj)));
            float4 v = LOAD4(Trace, clamp(j + dj, 0, CH - 1) * CW + clamp(i + di, 0, CW - 1));
            s.x += wgt * v.x; s.y += wgt * v.y; s.z += wgt * v.z; s.w += wgt * v.w;
        }
    float4 o = s * 0.0625;
    STORE4(Light, j * CW + i, o);
}
#endif

// ---------------------------------------------------- 6. blur (one direction)
#ifdef K_BLUR
StructuredBuffer<float> Src : register(t0, space0);
RWStructuredBuffer<float> Dst : register(u0, space1);
float tap(int k) { return Wt[k >> 2][k & 3]; }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int i = (int)id.x, j = (int)id.y;
    if (i >= SrcW || j >= SrcH) return;
    float4 c = LOAD4(Src, SrcOff + j * SrcW + i);
    float w0 = tap(0);
    float r = c.x * w0, g = c.y * w0, b = c.z * w0;
    for (int k = 1; k <= Radius; ++k) {
        int ia = clamp(i - k * DX, 0, SrcW - 1), ja = clamp(j - k * DY, 0, SrcH - 1);
        int ib = clamp(i + k * DX, 0, SrcW - 1), jb = clamp(j + k * DY, 0, SrcH - 1);
        float4 a = LOAD4(Src, SrcOff + ja * SrcW + ia), e = LOAD4(Src, SrcOff + jb * SrcW + ib);
        float wk = tap(k);
        r += wk * (a.x + e.x);
        g += wk * (a.y + e.y);
        b += wk * (a.z + e.z);
    }
    float4 o = float4(r, g, b, 0.0);
    STORE4(Dst, j * SrcW + i, o);
}
#endif

// ------------------------------------------------------------ 7. composite
#ifdef K_COMPOSITE
StructuredBuffer<uint> Albedo : register(t0, space0);
StructuredBuffer<uint> Emit : register(t1, space0);
StructuredBuffer<float> Light : register(t2, space0);
StructuredBuffer<float> BloomNear : register(t3, space0);
StructuredBuffer<float> BloomFar : register(t4, space0);
RWTexture2D<unorm float4> Out : register(u0, space1);

#define SAMPLE4(buf, gw, gh, gx, gy, outv)                                                         \
    {                                                                                              \
        float fx0_ = floor(gx), fy0_ = floor(gy);                                                  \
        int i0_ = (int)fx0_, j0_ = (int)fy0_;                                                      \
        int ia_ = clamp(i0_, 0, (gw) - 1), ib_ = clamp(i0_ + 1, 0, (gw) - 1);                      \
        int ja_ = clamp(j0_, 0, (gh) - 1), jb_ = clamp(j0_ + 1, 0, (gh) - 1);                      \
        outv = bilerp(LOAD4(buf, ja_ * (gw) + ia_), LOAD4(buf, ja_ * (gw) + ib_),                  \
                      LOAD4(buf, jb_ * (gw) + ia_), LOAD4(buf, jb_ * (gw) + ib_), (gx) - fx0_, (gy) - fy0_); \
    }

float shoulder(float x)
{
    const float k = 0.9;
    if (x <= k) return x;
    return k + (1.0 - k) * (1.0 - exp(-(x - k) / (1.0 - k)));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int x = (int)id.x, y = (int)id.y;
    if (x >= W || y >= H) return;
    int pi = y * W + x;
    float gy = ((float)y + 0.5) * InvC - 0.5;
    float fy = ((float)y + 0.5) * InvCF - 0.5;
    float gx = ((float)x + 0.5) * InvC - 0.5;
    float fx = ((float)x + 0.5) * InvCF - 0.5;
    int bw = mipW(BloomLevel), bh = mipH(BloomLevel);
    float4 L, Bn, Bf;
    SAMPLE4(Light, CW, CH, gx, gy, L);
    SAMPLE4(BloomNear, CW, CH, gx, gy, Bn);
    SAMPLE4(BloomFar, bw, bh, fx, fy, Bf);
    float sk = Sky * min(1.0, L.w * SkyGain);
    float lr = Ambient + sk * SkyR + L.x, lg = Ambient + sk * SkyG + L.y, lb = Ambient + sk * SkyB + L.z;
    uint a = Albedo[pi], e = Emit[pi];
    float ar = (float)(a & 0xFF) / 255.0, ag = (float)((a >> 8) & 0xFF) / 255.0, ab = (float)((a >> 16) & 0xFF) / 255.0;
    float hz = Haze * (1.0 - (float)(e >> 24) / 255.0);
    float orr = ar * ar * lr + hz * L.x + (float)(e & 0xFF) * EmitK + BloomN * Bn.x + BloomF * Bf.x;
    float og = ag * ag * lg + hz * L.y + (float)((e >> 8) & 0xFF) * EmitK + BloomN * Bn.y + BloomF * Bf.y;
    float ob = ab * ab * lb + hz * L.z + (float)((e >> 16) & 0xFF) * EmitK + BloomN * Bn.z + BloomF * Bf.z;
    Out[int2(x, y)] = float4(sqrt(saturate(shoulder(orr))), sqrt(saturate(shoulder(og))), sqrt(saturate(shoulder(ob))), 1.0);
}
#endif
