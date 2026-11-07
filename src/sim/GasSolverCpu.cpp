// CPU reference for the gas grid math. The GPU version (src/app/shaders/
// gas.hlsl) runs the same kernels in the same order; keep them in step.
//   1. curl       cell-centred vorticity
//   2. forces     buoyancy (heat + lightness; none in top-down cells, #483) and vorticity confinement, damping
//   3. advect     semi-Lagrangian advection of u and v
//   4. divergence of the advected field minus the expansion source
//   5. jacobi     pressure iterations (Neumann walls at obstacles)
//   6. project    subtract the pressure gradient
//   7. scale      per-cell limit so a cell never sends out more than it has
//   8. scalars    integer-exact donor-cell advection + diffusion of gas mass, and heat
#include "GasGrid.h"

#include <algorithm>
#include <cmath>

namespace notita {

void GasFields::resize(int nw, int nh, int nspecies) {
    w = nw;
    h = nh;
    species = nspecies;
    const std::size_t n = cells();
    open.assign(n, 0.0f);
    u.assign(static_cast<std::size_t>(w + 1) * h, 0.0f);
    v.assign(static_cast<std::size_t>(w) * (h + 1), 0.0f);
    p.assign(n, 0.0f);
    src.assign(n, 0.0f);
    mass.assign(n * static_cast<std::size_t>(std::max(species, 1)), 0);
    heat.assign(n, 0.0f);
}

float GasFields::capacity(int i, const GasSolveParams& sp) const {
    float c = std::max(open[i], 1.0f / kGasCellPx) * kGasCellPx * sp.airCap;
    for (int k = 0; k < species; ++k) c += static_cast<float>(m(k, i)) * (sp.cap[k] / kGasMassOne);
    return c;
}

float GasFields::speedAt(int i, int j) const {
    const float a = uc(i, j), b = vc(i, j);
    return std::sqrt(a * a + b * b);
}

namespace {

struct View {
    const GasFields& f;
    int w, h;
    bool openCell(int i, int j) const {
        return i >= 0 && j >= 0 && i < w && j < h && f.open[j * w + i] > 0.0f;
    }
    bool openU(int i, int j) const { return i > 0 && i < w && openCell(i - 1, j) && openCell(i, j); }
    bool openV(int i, int j) const { return j > 0 && j < h && openCell(i, j - 1) && openCell(i, j); }
};

float sampleU(const std::vector<float>& u, int w, int h, float x, float y) {
    // u sits at (i, j + 0.5)
    y -= 0.5f;
    x = std::clamp(x, 0.0f, static_cast<float>(w));
    y = std::clamp(y, 0.0f, static_cast<float>(h - 1));
    const int i0 = std::min(static_cast<int>(x), w - 1), j0 = std::min(static_cast<int>(y), std::max(h - 2, 0));
    const int i1 = std::min(i0 + 1, w), j1 = std::min(j0 + 1, h - 1);
    const float fx = x - static_cast<float>(i0), fy = y - static_cast<float>(j0);
    const int s = w + 1;
    const float a = u[j0 * s + i0] * (1 - fx) + u[j0 * s + i1] * fx;
    const float b = u[j1 * s + i0] * (1 - fx) + u[j1 * s + i1] * fx;
    return a * (1 - fy) + b * fy;
}

float sampleV(const std::vector<float>& v, int w, int h, float x, float y) {
    // v sits at (i + 0.5, j)
    x -= 0.5f;
    x = std::clamp(x, 0.0f, static_cast<float>(w - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(h));
    const int i0 = std::min(static_cast<int>(x), std::max(w - 2, 0)), j0 = std::min(static_cast<int>(y), h - 1);
    const int i1 = std::min(i0 + 1, w - 1), j1 = std::min(j0 + 1, h);
    const float fx = x - static_cast<float>(i0), fy = y - static_cast<float>(j0);
    const float a = v[j0 * w + i0] * (1 - fx) + v[j0 * w + i1] * fx;
    const float b = v[j1 * w + i0] * (1 - fx) + v[j1 * w + i1] * fx;
    return a * (1 - fy) + b * fy;
}

} // namespace

void CpuGasSolver::rows(int count, const std::function<void(int)>& fn) {
    if (pool_ && count >= 8) {
        // a few rows per job keeps the scheduling overhead low
        constexpr int kRowsPerJob = 4;
        const int jobs = (count + kRowsPerJob - 1) / kRowsPerJob;
        pool_->parallelFor(jobs, [&](int b) {
            for (int r = b * kRowsPerJob; r < std::min(count, (b + 1) * kRowsPerJob); ++r) fn(r);
        });
    } else {
        for (int r = 0; r < count; ++r) fn(r);
    }
}

void CpuGasSolver::solve(GasFields& f, const GasSolveParams& sp) {
    const int w = f.w, h = f.h, ns = f.species;
    if (w <= 0 || h <= 0) return;
    const std::size_t n = f.cells();
    const View view{f, w, h};
    curl_.assign(n, 0.0f);
    u2_.assign(f.u.size(), 0.0f);
    v2_.assign(f.v.size(), 0.0f);
    p2_.assign(n, 0.0f);
    div_.assign(n, 0.0f);
    scale_.assign(n, 1.0f);
    heat2_.assign(n, 0.0f);
    mass2_.assign(f.mass.size(), 0);

    auto ucOpen = [&](int i, int j) { return view.openCell(i, j) ? f.uc(i, j) : 0.0f; };
    auto vcOpen = [&](int i, int j) { return view.openCell(i, j) ? f.vc(i, j) : 0.0f; };

    // 1. curl
    rows(h, [&](int j) {
        for (int i = 0; i < w; ++i) {
            if (!view.openCell(i, j)) continue;
            curl_[j * w + i] = 0.5f * (vcOpen(i + 1, j) - vcOpen(i - 1, j)) - 0.5f * (ucOpen(i, j + 1) - ucOpen(i, j - 1));
        }
    });

    // 2. forces (each face owns its write; reads only curl / mass / heat)
    auto buoyancy = [&](int i, int j) {
        const int c = j * w + i;
        if (sp.flatCount > 0 && sp.flatCell(i, j)) return 0.0f; // #483: seen from above, nothing rises
        const float cap = f.capacity(c, sp);
        float b = sp.buoyancyHeat * (f.heat[c] / cap);
        const float perPx = 1.0f / (std::max(f.open[c], 1.0f / kGasCellPx) * kGasCellPx * kGasMassOne);
        for (int k = 0; k < ns; ++k) b += sp.buoyancyDensity * sp.light[k] * static_cast<float>(f.m(k, c)) * perPx;
        return b;
    };
    auto absCurl = [&](int i, int j) {
        i = std::clamp(i, 0, w - 1);
        j = std::clamp(j, 0, h - 1);
        return std::abs(curl_[j * w + i]);
    };
    auto vortForce = [&](int i, int j, float& fx, float& fy) {
        const float gx = 0.5f * (absCurl(i + 1, j) - absCurl(i - 1, j));
        const float gy = 0.5f * (absCurl(i, j + 1) - absCurl(i, j - 1));
        const float len = std::sqrt(gx * gx + gy * gy) + 1.0e-5f;
        const float wv = curl_[j * w + i];
        fx = sp.vorticity * (gy / len) * wv;
        fy = -sp.vorticity * (gx / len) * wv;
    };
    rows(h + 1, [&](int j) {
        for (int i = 0; i <= w; ++i) {
            if (j < h) {
                float& u = f.u[j * (w + 1) + i];
                if (!view.openU(i, j)) u = 0.0f;
                else {
                    float ax, ay, bx, by;
                    vortForce(i - 1, j, ax, ay);
                    vortForce(i, j, bx, by);
                    u = (u + 0.5f * (ax + bx)) * sp.damping;
                }
            }
            if (i < w) {
                float& v = f.v[j * w + i];
                if (!view.openV(i, j)) v = 0.0f;
                else {
                    float ax, ay, bx, by;
                    vortForce(i, j - 1, ax, ay);
                    vortForce(i, j, bx, by);
                    const float b = 0.5f * (buoyancy(i, j - 1) + buoyancy(i, j));
                    v = (v - b + 0.5f * (ay + by)) * sp.damping;
                }
            }
        }
    });

    // 3. advect velocity
    rows(h + 1, [&](int j) {
        for (int i = 0; i <= w; ++i) {
            if (j < h) {
                float r = 0.0f;
                if (view.openU(i, j)) {
                    const float x = static_cast<float>(i), y = static_cast<float>(j) + 0.5f;
                    const float vx = f.u[j * (w + 1) + i], vy = sampleV(f.v, w, h, x, y);
                    r = std::clamp(sampleU(f.u, w, h, x - vx, y - vy), -sp.maxSpeed, sp.maxSpeed);
                }
                u2_[j * (w + 1) + i] = r;
            }
            if (i < w) {
                float r = 0.0f;
                if (view.openV(i, j)) {
                    const float x = static_cast<float>(i) + 0.5f, y = static_cast<float>(j);
                    const float vx = sampleU(f.u, w, h, x, y), vy = f.v[j * w + i];
                    r = std::clamp(sampleV(f.v, w, h, x - vx, y - vy), -sp.maxSpeed, sp.maxSpeed);
                }
                v2_[j * w + i] = r;
            }
        }
    });

    // 4. divergence
    rows(h, [&](int j) {
        for (int i = 0; i < w; ++i) {
            const int c = j * w + i;
            div_[c] = view.openCell(i, j) ? (u2_[j * (w + 1) + i + 1] - u2_[j * (w + 1) + i] + v2_[(j + 1) * w + i] -
                                             v2_[j * w + i]) - f.src[c]
                                          : 0.0f;
        }
    });

    // 5. jacobi (ping-pong between p and p2)
    std::vector<float>* pin = &f.p;
    std::vector<float>* pout = &p2_;
    for (int it = 0; it < sp.pressureIters; ++it) {
        const std::vector<float>& P = *pin;
        std::vector<float>& Q = *pout;
        rows(h, [&](int j) {
            for (int i = 0; i < w; ++i) {
                const int c = j * w + i;
                if (!view.openCell(i, j)) { Q[c] = 0.0f; continue; }
                float sum = 0.0f;
                int cnt = 0;
                if (view.openCell(i - 1, j)) { sum += P[c - 1]; ++cnt; }
                if (view.openCell(i + 1, j)) { sum += P[c + 1]; ++cnt; }
                if (view.openCell(i, j - 1)) { sum += P[c - w]; ++cnt; }
                if (view.openCell(i, j + 1)) { sum += P[c + w]; ++cnt; }
                Q[c] = cnt > 0 ? (sum - div_[c]) / static_cast<float>(cnt) : 0.0f;
            }
        });
        std::swap(pin, pout);
    }
    if (pin != &f.p) f.p.swap(p2_);

    // 6. project
    rows(h + 1, [&](int j) {
        for (int i = 0; i <= w; ++i) {
            if (j < h) {
                const int s = j * (w + 1) + i;
                f.u[s] = view.openU(i, j)
                             ? std::clamp(u2_[s] - (f.p[j * w + i] - f.p[j * w + i - 1]), -sp.maxSpeed, sp.maxSpeed)
                             : 0.0f;
            }
            if (i < w) {
                const int s = j * w + i;
                f.v[s] = view.openV(i, j)
                             ? std::clamp(v2_[s] - (f.p[s] - f.p[s - w]), -sp.maxSpeed, sp.maxSpeed)
                             : 0.0f;
            }
        }
    });

    // 7. outflow scale
    rows(h, [&](int j) {
        for (int i = 0; i < w; ++i) {
            const int c = j * w + i;
            const float out = std::max(f.u[j * (w + 1) + i + 1], 0.0f) + std::max(-f.u[j * (w + 1) + i], 0.0f) +
                              std::max(f.v[(j + 1) * w + i], 0.0f) + std::max(-f.v[j * w + i], 0.0f);
            scale_[c] = out > 0.8f ? 0.8f / out : 1.0f;
        }
    });

    // 8. scalars: what leaves through a face is computed from the donor alone,
    //    by both cells, with the same expression, so it cancels exactly.
    const float d = std::clamp(sp.diffusion, 0.0f, 0.05f);
    rows(h, [&](int j) {
        for (int i = 0; i < w; ++i) {
            const int c = j * w + i;
            if (!view.openCell(i, j)) {
                for (int k = 0; k < ns; ++k) mass2_[static_cast<std::size_t>(k) * n + c] = f.m(k, c);
                heat2_[c] = f.heat[c];
                continue;
            }
            // faces: left, right, up, down. vel > 0 means flow towards +x / +y.
            const int nb[4] = {c - 1, c + 1, c - w, c + w};
            const bool ok[4] = {view.openCell(i - 1, j), view.openCell(i + 1, j), view.openCell(i, j - 1),
                                view.openCell(i, j + 1)};
            const float vel[4] = {f.u[j * (w + 1) + i], f.u[j * (w + 1) + i + 1], f.v[j * w + i], f.v[(j + 1) * w + i]};
            const float outSign[4] = {-1.0f, 1.0f, -1.0f, 1.0f}; // flow direction that leaves this cell
            for (int k = -1; k < ns; ++k) {
                // k == -1: heat (float); k >= 0: species mass (integer exact)
                if (k < 0) {
                    float hv = f.heat[c];
                    for (int q = 0; q < 4; ++q) {
                        if (!ok[q]) continue;
                        const float sv = vel[q] * outSign[q];
                        if (sv > 0.0f) hv -= f.heat[c] * (sv * scale_[c]);
                        else if (sv < 0.0f) hv += f.heat[nb[q]] * (-sv * scale_[nb[q]]);
                        // diffusion, written as a -> b with a the left / upper cell
                        if (q == 0 || q == 2) hv += (f.heat[nb[q]] - f.heat[c]) * d;
                        else hv -= (f.heat[c] - f.heat[nb[q]]) * d;
                    }
                    heat2_[c] = hv;
                    continue;
                }
                const std::int32_t me = f.m(k, c);
                std::int32_t mv = me;
                for (int q = 0; q < 4; ++q) {
                    if (!ok[q]) continue;
                    const std::int32_t other = f.m(k, nb[q]);
                    const float sv = vel[q] * outSign[q];
                    if (sv > 0.0f) mv -= gasAdvectFlux(me, sv, scale_[c]);
                    else if (sv < 0.0f) mv += gasAdvectFlux(other, -sv, scale_[nb[q]]);
                    if (q == 0 || q == 2) mv += gasDiffuseFlux(other, me, d); // left/up neighbour -> me
                    else mv -= gasDiffuseFlux(me, other, d);                  // me -> right/down neighbour
                }
                mass2_[static_cast<std::size_t>(k) * n + c] = mv;
            }
        }
    });
    f.mass.swap(mass2_);
    f.heat.swap(heat2_);
}

} // namespace notita
