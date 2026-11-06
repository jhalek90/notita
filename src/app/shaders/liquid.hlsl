// Liquid surface pass (#477), SDL3 GPU compute, entry point "main".
// Runs at output resolution (S output pixels per sim pixel) after the view
// is coloured and lit. Liquids are drawn as a smooth field instead of
// square pixels: a quadratic B-spline over the 3x3 sim pixels around each
// output pixel gives a metaball-like coverage, cut at a threshold with a
// soft edge. Inside, the colour is the coverage-weighted mean of the liquid
// pixels' (lit) colours, so the pool reads as one body; the top edge gets a
// surface line and a moving glint, the surface wobbles, the interior
// shimmers a little (refraction) and foam (falling streams, spray) gets a
// frothy white. Everything that isn't liquid is copied crisp (nearest).
// Bindings follow SDL's DXIL rules: read-only storage textures, then
// read-only storage buffers in t[n] space0; read-write storage textures in
// u[n] space1; uniforms in b0 space2.

cbuffer Params : register(b0, space2)
{
    int W; int H; int S; int Flags;      // sim view size, output scale, bit 0 = wobble
    float Time; float Pad0; float Pad1; float Pad2;
    float4 Shine[16];                    // surface line colour (rgb), strength (a)
    float4 Foam[16];                     // foam colour (rgb), strength (a)
    float4 Misc[16];                     // x wobble px, y shimmer px, z glint, w unused
};

Texture2D<unorm float4> Scene : register(t0, space0);
// per sim pixel: bits 0-7 look (0 = not liquid), 8-15 depth below the surface,
// 16-23 foam, 24 open (air or gas), 25 surface (liquid with an open cell above)
StructuredBuffer<uint> Info : register(t1, space0);
RWTexture2D<unorm float4> Out : register(u0, space1);

static const float Threshold = 0.42;
static const float Soft = 0.07;

uint info(int x, int y)
{
    if (x < 0 || y < 0 || x >= W || y >= H) return 0;
    return Info[y * W + x];
}

float3 scene(int x, int y)
{
    return Scene[int2(clamp(x, 0, W - 1), clamp(y, 0, H - 1))].rgb;
}

// quadratic B-spline, support |t| < 1.5, sums to 1 over integer shifts
float bspline(float t)
{
    t = abs(t);
    if (t < 0.5) return 0.75 - t * t;
    if (t < 1.5) { float u = 1.5 - t; return 0.5 * u * u; }
    return 0.0;
}

float hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float vnoise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i), b = hash21(i + float2(1, 0)), c = hash21(i + float2(0, 1)), d = hash21(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// Liquid coverage at sim position q, plus the weighted liquid colour, the
// strongest liquid tap's info and the weighted colour of what isn't liquid.
void field(float2 q, out float f, out float3 liq, out uint best, out float3 dry, out float dryW)
{
    int cx = (int)floor(q.x), cy = (int)floor(q.y);
    f = 0.0; liq = float3(0, 0, 0); dry = float3(0, 0, 0); dryW = 0.0;
    best = 0;
    float bw = -1.0;
    [unroll] for (int j = -1; j <= 1; ++j)
    {
        float wy = bspline(q.y - ((float)(cy + j) + 0.5));
        [unroll] for (int i = -1; i <= 1; ++i)
        {
            float w = wy * bspline(q.x - ((float)(cx + i) + 0.5));
            uint n = info(cx + i, cy + j);
            float3 c = scene(cx + i, cy + j);
            if ((n & 0xFF) != 0)
            {
                f += w;
                liq += w * c;
                if (w > bw) { bw = w; best = n; }
            }
            else
            {
                dry += w * c;
                dryW += w;
            }
        }
    }
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int ox = (int)id.x, oy = (int)id.y;
    if (ox >= W * S || oy >= H * S) return;
    float invS = 1.0 / (float)S;
    float2 p = (float2((float)ox, (float)oy) + 0.5) * invS;
    int px = (int)floor(p.x), py = (int)floor(p.y);
    uint here = info(px, py);
    float3 crisp = scene(px, py);

    // cheap out: nothing liquid in the 3x3 around this pixel
    bool any = false;
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i)
            any = any || (info(px + i, py + j) & 0xFF) != 0;
    if (!any) { Out[int2(ox, oy)] = float4(crisp, 1.0); return; }

    // the surface wobbles where liquid meets open air (not against walls or floors)
    uint look0 = here & 0xFF;
    uint below = info(px, py + 1);
    bool nearTop = (here & (1u << 25)) != 0 || ((here & (1u << 24)) != 0 && (below & (1u << 25)) != 0);
    uint lk = look0 != 0 ? look0 : (below & 0xFF);
    float2 q = p;
    if ((Flags & 1) != 0 && nearTop && lk != 0)
    {
        float a = Misc[lk].x;
        q.y += a * (0.6 * sin(p.x * 0.55 + Time * 2.3) + 0.4 * sin(p.x * 0.23 - Time * 1.4));
    }

    float f, dryW;
    float3 liq, dry;
    uint best;
    field(q, f, liq, best, dry, dryW);
    if (f <= Threshold - Soft) { Out[int2(ox, oy)] = float4(crisp, 1.0); return; }

    uint look = best & 0xFF;
    float3 col = liq / max(f, 1e-4);

    // refraction shimmer: the interior colour is sampled a hair off, moving slowly
    float depth = (float)((best >> 8) & 0xFF);
    if ((Flags & 1) != 0 && Misc[look].y > 0.0 && depth > 1.0)
    {
        float2 r = float2(sin(p.y * 0.8 + Time * 1.7), cos(p.x * 0.7 - Time * 1.3)) * Misc[look].y;
        float f2, d2w;
        float3 l2, d2;
        uint b2;
        field(q + r, f2, l2, b2, d2, d2w);
        if (f2 > 0.5) col = lerp(col, l2 / f2, 0.6);
    }

    // surface line: inside here, outside a little higher up, and open air above
    float f3, d3w;
    float3 l3, d3;
    uint b3;
    float lineW = 0.85;
    field(q - float2(0.0, lineW), f3, l3, b3, d3, d3w);
    bool openAbove = (info(px, (int)floor(q.y - lineW)) & ((1u << 24) | 0xFF)) != 0;
    if (f3 < Threshold && openAbove)
    {
        float t = saturate((Threshold - f3) / 0.25);
        float4 sh = Shine[look];
        float glint = pow(saturate(sin(p.x * 0.31 + Time * 0.9) * sin(p.x * 0.17 - Time * 0.6 + 1.3)), 6.0) * Misc[look].z;
        col = lerp(col, sh.rgb, saturate(sh.a * t + glint * t));
    }

    // foam: falling streams and spray, frothy
    float foam = (float)((best >> 16) & 0xFF) / 255.0;
    if (foam > 0.0)
    {
        float n = vnoise(p * 1.7 + float2(0.0, -Time * 3.0)) * 0.6 + vnoise(p * 4.1 + float2(Time, 0.0)) * 0.4;
        float4 fo = Foam[look];
        col = lerp(col, fo.rgb, saturate(foam * fo.a * (0.55 + 0.9 * n)));
    }

    // soft edge against what isn't liquid (the dry neighbours, or the crisp pixel)
    float3 outside = look0 == 0 ? crisp : (dryW > 1e-4 ? dry / dryW : crisp);
    float a = smoothstep(Threshold - Soft, Threshold + Soft, f);
    Out[int2(ox, oy)] = float4(lerp(outside, col, a), 1.0);
}
