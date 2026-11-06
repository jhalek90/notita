// Phase 6 tests: the view lighting. They run CpuLight, the reference the GPU
// kernels (src/app/shaders/light.hlsl) mirror, so none of this needs a GPU.
#include "test.h"

#include "light/Lighting.h"
#include "sim/ThreadPool.h"

#include <cmath>
#include <cstdio>

using namespace notita;

namespace {

constexpr int kW = 160, kH = 96;

LightInputs blank(float sky = 0.0f) {
    LightInputs in;
    in.resize(kW, kH);
    for (auto& a : in.albedo) a = 0xFF808080u;
    for (float& s : in.topSky) s = sky;
    return in;
}

void put(LightInputs& in, int x, int y, float r, float g, float b, float opacity) {
    in.emit[static_cast<std::size_t>(y) * kW + x] = packEmit(r, g, b, opacity);
}

void lamp(LightInputs& in, int cx, int cy, float v) {
    for (int y = cy - 2; y < cy + 2; ++y)
        for (int x = cx - 2; x < cx + 2; ++x) put(in, x, y, v, v, v, 0.0f);
}

// light (green channel) at a pixel, from the cell field after the tent
float lightAt(const CpuLight& L, int x, int y, int cellPx = 2) {
    const LightFields& f = L.fields();
    const int c = (y / cellPx) * f.cw + x / cellPx;
    return f.light[static_cast<std::size_t>(c) * 4 + 1];
}

float skyAt(const CpuLight& L, int x, int y, int cellPx = 2) {
    const LightFields& f = L.fields();
    const int c = (y / cellPx) * f.cw + x / cellPx;
    return f.light[static_cast<std::size_t>(c) * 4 + 3];
}

LightParams noBloom() {
    LightParams p;
    p.bloom = 0.0f;
    return p;
}

} // namespace

TEST_CASE(light_wall_casts_shadow) {
    // a lamp left of a solid wall: its side is lit, behind the wall stays dark
    LightInputs in = blank();
    for (int y = 30; y < 86; ++y)
        for (int x = 78; x < 84; ++x) put(in, x, y, 0, 0, 0, 1.0f);
    lamp(in, 50, 48, 3.0f);
    CpuLight L;
    std::vector<std::uint32_t> out;
    L.run(in, noBloom(), out);
    const float near = lightAt(L, 70, 48), far = lightAt(L, 92, 48);
    std::printf("    near %.4f far %.4f\n", near, far);
    CHECK(near > 0.02f);
    CHECK(far < near * 0.05f);
    // over the wall's top the lamp is in plain view again
    const float over = lightAt(L, 100, 4);
    std::printf("    over the wall %.4f\n", over);
    CHECK(over > 0.01f);
    // the pixels show it too
    CHECK((out[48 * kW + 70] & 0xFF) > (out[48 * kW + 92] & 0xFF));
}

TEST_CASE(light_falls_off_with_distance) {
    LightInputs in = blank();
    lamp(in, 20, 48, 3.0f);
    CpuLight L;
    std::vector<std::uint32_t> out;
    L.run(in, noBloom(), out);
    const float a = lightAt(L, 30, 48), b = lightAt(L, 60, 48), c = lightAt(L, 110, 48), d = lightAt(L, 150, 48);
    std::printf("    10px %.4f 40px %.4f 90px %.4f 130px %.4f\n", a, b, c, d);
    CHECK(a > b);
    CHECK(b > c);
    CHECK(c > d);
    CHECK(d < a * 0.1f);
    // it is roughly the same in every direction (the rays are spread evenly)
    const float up = lightAt(L, 20, 18), right = lightAt(L, 50, 48);
    std::printf("    30 px up %.4f, 30 px right %.4f\n", up, right);
    CHECK(std::abs(up - right) < 0.35f * std::max(up, right));
}

TEST_CASE(light_translucent_liquid_dims_light) {
    LightInputs in = blank();
    lamp(in, 20, 48, 3.0f);
    LightInputs water = in;
    for (int y = 0; y < kH; ++y)
        for (int x = 40; x < 60; ++x) put(water, x, y, 0, 0, 0, 0.15f);
    CpuLight a, b;
    std::vector<std::uint32_t> out;
    a.run(in, noBloom(), out);
    b.run(water, noBloom(), out);
    const float open = lightAt(a, 80, 48), through = lightAt(b, 80, 48);
    std::printf("    open %.4f through 20 px of liquid %.4f\n", open, through);
    CHECK(through < open * 0.6f);
    CHECK(through > open * 0.001f); // dimmer, but not a wall
}

TEST_CASE(light_sky_lights_open_air_not_caves) {
    // ground at the bottom, a sealed cave inside it; the sky comes in from the top
    LightInputs in = blank(1.0f);
    for (int y = 50; y < kH; ++y)
        for (int x = 0; x < kW; ++x) put(in, x, y, 0, 0, 0, 1.0f);
    for (int y = 64; y < 84; ++y)
        for (int x = 60; x < 100; ++x) put(in, x, y, 0, 0, 0, 0.0f);
    CpuLight L;
    std::vector<std::uint32_t> out;
    LightParams p = noBloom();
    L.run(in, p, out);
    const float air = skyAt(L, 80, 20), floor = skyAt(L, 80, 48), cave = skyAt(L, 80, 74);
    std::printf("    sky: open air %.3f, at the ground %.3f, sealed cave %.4f\n", air, floor, cave);
    CHECK(air > 0.45f); // 30 px over the ground: the lower half of the rays hit it
    CHECK(floor > 0.25f && floor < air);
    CHECK(cave < 0.01f);
    // a covered top (underground view) brings no sky at all
    LightInputs under = in;
    for (float& s : under.topSky) s = 0.0f;
    L.run(under, p, out);
    CHECK(skyAt(L, 80, 20) < 0.01f);
}

TEST_CASE(light_no_nans_on_noise) {
    LightInputs in = blank(0.5f);
    std::uint32_t h = 12345;
    for (std::size_t i = 0; i < in.emit.size(); ++i) {
        h = h * 1664525u + 1013904223u;
        in.emit[i] = h;
        in.albedo[i] = 0xFF000000u | (h >> 8);
    }
    CpuLight L;
    std::vector<std::uint32_t> out;
    LightParams p;
    p.gain = 4.0f;
    p.bloom = 2.0f;
    L.run(in, p, out);
    const LightFields& f = L.fields();
    bool finite = true;
    for (const auto* v : {&f.mip, &f.sky, &f.trace, &f.light, &f.bloomNear, &f.bloomFar})
        for (float x : *v) finite = finite && std::isfinite(x) && x >= 0.0f;
    CHECK(finite);
    CHECK_EQ(out.size(), static_cast<std::size_t>(kW * kH));
}

TEST_CASE(light_deterministic_any_threads) {
    LightInputs in = blank(1.0f);
    for (int y = 60; y < kH; ++y)
        for (int x = 0; x < kW; ++x) put(in, x, y, (x % 17 == 0) ? 2.0f : 0.0f, 0.3f, 0.0f, 0.6f);
    lamp(in, 100, 30, 2.5f);
    CpuLight a, b, c;
    std::vector<std::uint32_t> oa, ob, oc;
    ThreadPool one(1), many(4);
    a.run(in, LightParams{}, oa, nullptr);
    b.run(in, LightParams{}, ob, &one);
    c.run(in, LightParams{}, oc, &many);
    CHECK(oa == ob);
    CHECK(oa == oc);
    CHECK(a.fields().light == c.fields().light);
    // and the CPU preset (4 px cells, fewer rays) too
    LightParams q;
    q.cellPx = 4; q.dirs = 16; q.range = 48;
    a.run(in, q, oa, nullptr);
    c.run(in, q, oc, &many);
    CHECK(oa == oc);
}

TEST_CASE(light_composite_without_lights_is_albedo_times_ambient) {
    // no emission, no sky: out = sqrt(albedo^2 * ambient) (gamma-2 linear space),
    // below the tonemap's knee (0.9 linear, about 242 / 255) where it is exact
    LightInputs in = blank(0.0f);
    for (std::size_t i = 0; i < in.albedo.size(); ++i) {
        const std::uint32_t v = static_cast<std::uint32_t>(i * 37 % 256);
        in.albedo[i] = 0xFF000000u | v | ((255 - v) << 8) | ((v / 2) << 16);
    }
    for (auto& e : in.emit) e = packEmit(0, 0, 0, 0.6f);
    for (float amb : {1.0f, 0.3f}) {
        LightParams p;
        p.ambient = amb;
        p.sky = 0.0f;
        CpuLight L;
        std::vector<std::uint32_t> out;
        L.run(in, p, out);
        int maxd = 0;
        for (std::size_t i = 0; i < out.size(); ++i)
            for (int k = 0; k < 3; ++k) {
                const float a = static_cast<float>((in.albedo[i] >> (8 * k)) & 0xFF) / 255.0f;
                if (a * a * amb > 0.9f) continue;
                const int want = static_cast<int>(std::sqrt(a * a * amb) * 255.0f + 0.5f);
                maxd = std::max(maxd, std::abs(want - static_cast<int>((out[i] >> (8 * k)) & 0xFF)));
            }
        std::printf("    ambient %.1f: max diff %d\n", amb, maxd);
        CHECK(maxd <= 1);
    }
}

TEST_CASE(light_bloom_spreads_only_from_emitters) {
    LightInputs in = blank(0.0f);
    for (auto& e : in.emit) e = packEmit(0, 0, 0, 1.0f); // all solid: no traced light reaches anything
    put(in, 80, 48, 4.0f, 4.0f, 4.0f, 1.0f);
    LightParams p;
    p.ambient = 0.0f;
    p.sky = 0.0f;
    CpuLight L;
    std::vector<std::uint32_t> withBloom, without;
    L.run(in, p, withBloom);
    p.bloom = 0.0f;
    L.run(in, p, without);
    CHECK((withBloom[48 * kW + 90] & 0xFF) > (without[48 * kW + 90] & 0xFF)); // a halo next to it
    CHECK((without[48 * kW + 90] & 0xFF) == 0);
    CHECK((withBloom[10 * kW + 10] & 0xFF) == 0); // nothing far away
}
