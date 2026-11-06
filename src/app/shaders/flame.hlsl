// Flame pass (#480), SDL3 GPU compute, entry point "main".
// Flames are hot, reacting gas on the gas grid (one cell per 4x4 sim px).
// Runs at output resolution after the scene is lit (and after the liquid
// pass): per output pixel the grid fields are sampled with a cubic B-spline
// (smooth, no 4x4 blocks), through a noise warp that scrolls upwards so the
// edges lick and flicker. The glow comes from the temperature (a blackbody
// ramp: deep red, orange, yellow, white, white-blue when very hot), boosted
// where the gas is burning right now, added on top of the scene with a soft
// shoulder; soot darkens the scene where it is not glowing. Per-fuel tints
// (e.g. hydrogen burns pale blue) blend in by the cell's tint amount.
// The CPU fallback and the light emission in AppFlame.cpp use the same ramp.
// Bindings follow SDL's DXIL rules: read-only storage textures, then
// read-only storage buffers in t[n] space0; read-write storage textures in
// u[n] space1; uniforms in b0 space2.

cbuffer Params : register(b0, space2)
{
    int W; int H; int S; int Flags;      // sim view size, output scale
    int SW; int SH; int CW; int CH;      // scene texture size, cells over the view
    float OffX; float OffY; float Time; float Gain;
    float Smoke; float Flicker; float MinTemp; float Pad;
    float OrgX; float OrgY; float Pad1; float Pad2; // world grid cell of the buffer's cell (0, 0), for the noise
    float4 Tint[16];                     // rgb, strength
};

Texture2D<unorm float4> Scene : register(t0, space0);
// x temperature C, y burn rate, z soot, w tint index + amount
StructuredBuffer<float4> Cells : register(t1, space0);
RWTexture2D<unorm float4> Out : register(u0, space1);

float4 cell(int i, int j)
{
    if (i < 0 || j < 0 || i >= CW || j >= CH) return float4(20.0, 0.0, 0.0, 0.0);
    return Cells[j * CW + i];
}

void bsplineWeights(float f, out float4 w)
{
    const float f2 = f * f, f3 = f2 * f;
    w.x = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    w.y = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    w.z = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    w.w = f3 / 6.0;
}

// grid coordinate g: cell centres at integers
float4 sampleCells(float2 g, out float tintIdx)
{
    const float2 b = floor(g);
    const float2 f = g - b;
    float4 wx, wy;
    bsplineWeights(f.x, wx);
    bsplineWeights(f.y, wy);
    const int bi = (int)b.x - 1, bj = (int)b.y - 1;
    float4 s = 0;
    float best = 0.0;
    tintIdx = 0.0;
    [unroll] for (int n = 0; n < 4; ++n) {
        const float wyn = wy[n];
        [unroll] for (int m = 0; m < 4; ++m) {
            const float4 c = cell(bi + m, bj + n);
            const float w = wx[m] * wyn;
            s += c * w;
            // the tint index isn't interpolated: take the strongest tinted cell's
            const float amt = frac(c.w) * w;
            if (amt > best) { best = amt; tintIdx = floor(c.w); }
        }
    }
    s.w = saturate(best * 2.5);
    return s;
}

float hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float vnoise(float2 p)
{
    const float2 i = floor(p);
    float2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    const float a = hash21(i), b = hash21(i + float2(1, 0)), c = hash21(i + float2(0, 1)), d = hash21(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float fbm(float2 p)
{
    return 0.55 * vnoise(p) + 0.3 * vnoise(p * 2.03 + 17.1) + 0.15 * vnoise(p * 4.1 + 31.7);
}

// blackbody-ish ramp, display space (keep in step with flameRamp in AppFlame.cpp)
float3 blackbody(float t)
{
    static const float4 stops[7] = {
        float4(500.0, 0.30, 0.02, 0.00), float4(750.0, 0.80, 0.10, 0.01), float4(1000.0, 1.00, 0.30, 0.03),
        float4(1250.0, 1.00, 0.52, 0.10), float4(1500.0, 1.00, 0.75, 0.30), float4(1900.0, 1.00, 0.93, 0.75),
        float4(2600.0, 0.80, 0.90, 1.25),
    };
    if (t <= stops[0].x) return stops[0].yzw;
    [unroll] for (int k = 1; k < 7; ++k)
        if (t <= stops[k].x) return lerp(stops[k - 1].yzw, stops[k].yzw, (t - stops[k - 1].x) / (stops[k].x - stops[k - 1].x));
    return stops[6].yzw;
}

float soft(float x) { return x < 0.8 ? x : 0.8 + 0.2 * (1.0 - exp(-(x - 0.8) / 0.2)); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int ox = (int)id.x, oy = (int)id.y;
    if (ox >= W * S || oy >= H * S) return;
    const int sx = ox * SW / (W * S), sy = oy * SH / (H * S);
    float3 col = Scene[int2(sx, sy)].rgb;

    // sim pixel position, then grid coordinates
    const float2 sp = (float2(ox, oy) + 0.5) / (float)S;
    float2 g = sp * 0.25 + float2(OffX, OffY);
    // flicker: a warp that scrolls upwards (grid space, so it doesn't swim with the camera)
    const float2 wg = g + float2(OrgX, OrgY);
    const float2 n = float2(fbm(wg * 0.8 + float2(3.1, Time * 1.6)), fbm(wg * 0.8 + float2(11.7, Time * 1.9)));
    g += (n - 0.5) * (1.6 * Flicker);
    float tintIdx;
    const float4 s = sampleCells(g, tintIdx);
    const float t = s.x, burn = max(s.y, 0.0), soot = max(s.z, 0.0);

    const float heat = saturate((t - MinTemp) / 900.0);
    const float react = saturate(burn * 0.06);
    const int ti = clamp((int)tintIdx, 0, 15);
    const float tinted = s.w * Tint[ti].a;
    // tinted (gas) flames are thin and pale: dimmer than a sooty wood flame
    float I = min(heat * sqrt(heat) * (0.15 + 0.85 * react), 1.1) * (1.0 - 0.6 * tinted);
    // fine flicker inside the flame (faster, smaller)
    // stretched upwards, so it reads as tongues rather than a cloud
    const float fine = fbm(wg * float2(2.4, 0.9) + float2(0.0, Time * 3.5));
    I *= 1.0 + Flicker * (fine - 0.5) * 1.2;
    I *= smoothstep(0.02, 0.18, I); // soft edge: the thin outer haze fades out
    float3 fl = blackbody(t + 320.0 * react); // the burning core runs yellow
    fl = lerp(fl, Tint[ti].rgb * (0.6 + 0.6 * heat), tinted);
    fl = lerp(fl, float3(1.0, 1.0, 1.0), saturate(I - 1.2) * 0.4); // the hottest core whitens

    // soot over the scene where it isn't glowing
    const float dark = (1.0 - exp(-soot * 0.2 * Smoke)) * 0.45 * (1.0 - saturate(I * 3.0));
    col *= 1.0 - dark;
    col += fl * (I * Gain);
    Out[int2(ox, oy)] = float4(soft(col.r), soft(col.g), soft(col.b), 1.0);
}
