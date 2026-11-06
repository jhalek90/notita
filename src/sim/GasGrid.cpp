#include "GasGrid.h"

#include "Particles.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <type_traits>

namespace notita {

namespace {

bool openKind(MaterialKind k) {
    return k == MaterialKind::Empty || k == MaterialKind::Gas || k == MaterialKind::Special;
}
bool looseKind(MaterialKind k) { return k == MaterialKind::Liquid || k == MaterialKind::Powder; }

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

GasGrid::GasGrid(World& world, const MaterialRegistry& mats, ThreadPool* pool, std::uint64_t seed)
    : world_(world), mats_(mats), pool_(pool), seed_(seed), cpu_(pool) {
    speciesIndex_.assign(mats.size(), -1);
    for (const Material& m : mats.all())
        if (m.kind == MaterialKind::Gas && m.mass > 0 && static_cast<int>(species_.size()) < kMaxGasSpecies) {
            speciesIndex_[m.id] = static_cast<int>(species_.size());
            species_.push_back(m.id);
        }
    ox_ = world.minX();
    oy_ = world.minY();
    const int w = (world.widthPx() + kGasCell - 1) / kGasCell;
    const int h = (world.heightPx() + kGasCell - 1) / kGasCell;
    f_.resize(std::max(w, 0), std::max(h, 0), speciesCount());
    sp_.w = f_.w;
    sp_.h = f_.h;
    sp_.species = speciesCount();
    const float airDensity = mats[kEmpty].density;
    sp_.airCap = mats[kEmpty].heatCapacity;
    for (int k = 0; k < speciesCount(); ++k) {
        const Material& m = mats[species_[k]];
        sp_.light[k] = std::clamp((airDensity - m.density) / airDensity, -4.0f, 1.0f);
        sp_.cap[k] = m.heatCapacity / static_cast<float>(m.mass);
    }
    flameIn_.assign(f_.cells(), 0);
    loose_.assign(f_.cells(), 0);
    extraSrc_.assign(f_.cells(), 0.0f);
    burnt_.assign(f_.cells(), 0);
    // #480: flames
    const std::size_t n = f_.cells();
    fuel_.assign(n, 0.0f);
    soot_.assign(n, 0.0f);
    flameT_.assign(n, 0.0f);
    glow_.assign(n, 0.0f);
    tintE_.assign(n, 0.0f);
    tintMat_.assign(n, kEmpty);
    litPrev_.assign(n, 0);
    auto extraF = [](const Material& m, const char* key, float def) {
        auto it = m.extra.find(key);
        return it == m.extra.end() ? def : std::strtof(it->second.c_str(), nullptr);
    };
    flameFuelOf_.assign(mats.size(), 0.0f);
    for (const Material& m : mats.all())
        if (m.kind == MaterialKind::Special) flameFuelOf_[m.id] = std::max(0.0f, extraF(m, "flame_fuel", 0.0f));
    flameTempOf_.assign(static_cast<std::size_t>(speciesCount()), 0.0f);
    for (int k = 0; k < speciesCount(); ++k) {
        const Material& m = mats[species_[k]];
        flameTempOf_[static_cast<std::size_t>(k)] = extraF(m, "flame_temp", m.burnMaxTemp > 0.0f ? m.burnMaxTemp : 0.0f);
        if (m.name == "oxygen") oxygen_ = k;
    }
}

void GasGrid::buildChunkTable() {
    tcx0_ = ox_ >> kChunkShift;
    tcy0_ = oy_ >> kChunkShift;
    tcw_ = (f_.w * kGasCell + kChunkSize - 1) / kChunkSize;
    tch_ = (f_.h * kGasCell + kChunkSize - 1) / kChunkSize;
    table_.assign(static_cast<std::size_t>(tcw_) * tch_, nullptr);
    for (int cy = 0; cy < tch_; ++cy)
        for (int cx = 0; cx < tcw_; ++cx) table_[static_cast<std::size_t>(cy) * tcw_ + cx] = world_.chunk(tcx0_ + cx, tcy0_ + cy);
}

void GasGrid::wakePx(int x, int y) const {
    const int x0 = x - 1, y0 = y - 1, x1 = x + 1, y1 = y + 1;
    for (int cy = y0 >> kChunkShift; cy <= y1 >> kChunkShift; ++cy)
        for (int cx = x0 >> kChunkShift; cx <= x1 >> kChunkShift; ++cx) {
            Chunk* c = chunkAt(cx << kChunkShift, cy << kChunkShift);
            if (!c) continue;
            const int ox = cx * kChunkSize, oy = cy * kChunkSize;
            c->nextRect.include(std::max(x0 - ox, 0), std::max(y0 - oy, 0), std::min(x1 - ox, kChunkMask),
                                std::min(y1 - oy, kChunkMask));
            c->heatNext.store(true, std::memory_order_relaxed);
        }
}

Rng GasGrid::cellRng(int i, std::uint64_t salt) const {
    return Rng(Rng::hash(Rng::hash(seed_ ^ 0x6A5D39EAull, tick_ * 8 + salt), static_cast<std::uint64_t>(i)));
}

float GasGrid::temperature(int i) const { return kAmbientTemp + f_.heat[i] / f_.capacity(i, sp_); }

std::int64_t GasGrid::massFixed(int k) const {
    std::int64_t s = 0;
    const std::size_t n = f_.cells();
    for (std::size_t i = 0; i < n; ++i) s += f_.mass[static_cast<std::size_t>(k) * n + i];
    return s;
}
std::int64_t GasGrid::massFixed() const {
    std::int64_t s = 0;
    for (int k = 0; k < speciesCount(); ++k) s += massFixed(k);
    return s;
}
double GasGrid::heatEnergy() const {
    double s = 0;
    for (float hv : f_.heat) s += hv;
    return s;
}

void GasGrid::absorb(const GasEmission* e, std::size_t count) {
    if (f_.w <= 0) return;
    for (std::size_t q = 0; q < count; ++q) {
        const int i = std::clamp((e[q].x - ox_) / kGasCell, 0, f_.w - 1);
        const int j = std::clamp((e[q].y - oy_) / kGasCell, 0, f_.h - 1);
        const int c = j * f_.w + i;
        if (e[q].fuel > 0.0f && e[q].mass == 0) { // #480: fuel vapour from a burning pixel
            fuel_[static_cast<std::size_t>(c)] += e[q].fuel;
            flameFields_ = true;
            continue;
        }
        const int k = speciesOf(e[q].material);
        if (k < 0) continue;
        f_.m(k, c) += e[q].mass * kGasMassOne;
        // the pixel became air at the same temperature; the grid takes the rest of the gas's heat
        f_.heat[c] += (mats_[e[q].material].heatCapacity - sp_.airCap) * (e[q].temperature - kAmbientTemp);
        absorbedPending_ += e[q].mass;
    }
}

void GasGrid::clear() {
    kicks_.clear();
    lostHeat_ += heatEnergy();
    std::fill(f_.u.begin(), f_.u.end(), 0.0f);
    std::fill(f_.v.begin(), f_.v.end(), 0.0f);
    std::fill(f_.p.begin(), f_.p.end(), 0.0f);
    std::fill(f_.mass.begin(), f_.mass.end(), 0);
    std::fill(f_.heat.begin(), f_.heat.end(), 0.0f);
    std::fill(extraSrc_.begin(), extraSrc_.end(), 0.0f);
    std::fill(burnt_.begin(), burnt_.end(), std::uint8_t{0});
    for (auto* v : {&fuel_, &soot_, &flameT_, &glow_, &tintE_}) std::fill(v->begin(), v->end(), 0.0f);
    std::fill(litPrev_.begin(), litPrev_.end(), std::uint8_t{0});
    blasts_.clear();
}

double GasGrid::fuelEnergy() const {
    double s = 0;
    for (float v : fuel_) s += v;
    return s;
}

// --------------------------------------------------------------------------
// phase 6: streaming

std::int64_t GasGrid::ChunkCells::massFixed() const {
    std::int64_t s = 0;
    for (std::int32_t m : mass) s += m;
    return s;
}

bool GasGrid::exportChunk(int cx, int cy, ChunkCells& out, bool clear) {
    constexpr int S = kChunkCellsSide;
    const int i0 = (cx * kChunkSize - ox_) / kGasCell, j0 = (cy * kChunkSize - oy_) / kGasCell;
    if (i0 < 0 || j0 < 0 || i0 + S > f_.w || j0 + S > f_.h) return false;
    const int ns = speciesCount();
    out.species = ns;
    out.mass.assign(static_cast<std::size_t>(ns) * S * S, 0);
    out.heat.assign(S * S, 0.0f);
    out.u.assign(S * S, 0.0f);
    out.v.assign(S * S, 0.0f);
    out.p.assign(S * S, 0.0f);
    out.extra.assign(S * S, 0.0f);
    out.open.assign(S * S, 0.0f);
    out.src.assign(S * S, 0.0f);
    out.burnt.assign(S * S, 0);
    out.fuel.assign(S * S, 0.0f);
    out.soot.assign(S * S, 0.0f);
    const int w = f_.w;
    for (int j = 0; j < S; ++j)
        for (int i = 0; i < S; ++i) {
            const int c = (j0 + j) * w + (i0 + i), l = j * S + i;
            for (int k = 0; k < ns; ++k) out.mass[static_cast<std::size_t>(k) * S * S + l] = f_.m(k, c);
            out.heat[l] = f_.heat[c];
            out.u[l] = f_.u[(j0 + j) * (w + 1) + i0 + i];
            out.v[l] = f_.v[c];
            out.p[l] = f_.p[c];
            out.extra[l] = extraSrc_[c];
            out.open[l] = f_.open[c];
            out.src[l] = f_.src[c];
            out.burnt[l] = litPrev_[c]; // after a step the last burn map is in litPrev_
            out.fuel[l] = fuel_[c];
            out.soot[l] = soot_[c];
            if (clear) {
                fuel_[c] = soot_[c] = flameT_[c] = glow_[c] = tintE_[c] = 0.0f;
                for (int k = 0; k < ns; ++k) f_.m(k, c) = 0;
                f_.heat[c] = 0.0f;
                f_.u[(j0 + j) * (w + 1) + i0 + i] = 0.0f;
                f_.v[c] = 0.0f;
                f_.p[c] = 0.0f;
                extraSrc_[c] = 0.0f;
                burnt_[c] = 0;
                litPrev_[c] = 0;
            }
        }
    return true;
}

bool GasGrid::importChunk(int cx, int cy, const ChunkCells& in) {
    constexpr int S = kChunkCellsSide;
    const int i0 = (cx * kChunkSize - ox_) / kGasCell, j0 = (cy * kChunkSize - oy_) / kGasCell;
    if (i0 < 0 || j0 < 0 || i0 + S > f_.w || j0 + S > f_.h || in.empty()) return false;
    const int ns = std::min(in.species, speciesCount());
    const int w = f_.w;
    for (int j = 0; j < S; ++j)
        for (int i = 0; i < S; ++i) {
            const int c = (j0 + j) * w + (i0 + i), l = j * S + i;
            for (int k = 0; k < ns; ++k) f_.m(k, c) += in.mass[static_cast<std::size_t>(k) * S * S + l];
            f_.heat[c] += in.heat[l];
            f_.u[(j0 + j) * (w + 1) + i0 + i] = in.u[l];
            f_.v[c] = in.v[l];
            f_.p[c] = in.p[l];
            extraSrc_[c] = in.extra[l];
            if (!in.open.empty()) f_.open[c] = in.open[l];
            if (!in.src.empty()) f_.src[c] = in.src[l];
            litPrev_[c] = in.burnt[l];
            if (!in.fuel.empty()) fuel_[c] = in.fuel[l];
            if (!in.soot.empty()) soot_[c] = in.soot[l];
        }
    flameFields_ = true;
    return true;
}

std::int64_t GasGrid::rebase(int originX, int originY) {
    const int di = (originX - ox_) / kGasCell, dj = (originY - oy_) / kGasCell;
    if (di == 0 && dj == 0) return 0;
    const int w = f_.w, h = f_.h, ns = speciesCount();
    const std::size_t n = f_.cells();
    std::int64_t dropped = massFixed();
    auto shiftCells = [&](auto& a, std::size_t planes) {
        using T = typename std::decay_t<decltype(a)>::value_type;
        std::vector<T> b(a.size(), T{});
        for (std::size_t k = 0; k < planes; ++k)
            for (int j = 0; j < h; ++j) {
                const int sj = j + dj;
                if (sj < 0 || sj >= h) continue;
                for (int i = 0; i < w; ++i) {
                    const int si = i + di;
                    if (si < 0 || si >= w) continue;
                    b[k * n + static_cast<std::size_t>(j) * w + i] = a[k * n + static_cast<std::size_t>(sj) * w + si];
                }
            }
        a.swap(b);
    };
    shiftCells(f_.mass, static_cast<std::size_t>(ns));
    shiftCells(f_.heat, 1);
    shiftCells(f_.p, 1);
    shiftCells(f_.open, 1);
    shiftCells(f_.src, 1);
    shiftCells(extraSrc_, 1);
    shiftCells(burnt_, 1);
    shiftCells(flameIn_, 1);
    shiftCells(loose_, 1);
    shiftCells(fuel_, 1);
    shiftCells(soot_, 1);
    shiftCells(flameT_, 1);
    shiftCells(glow_, 1);
    shiftCells(tintE_, 1);
    shiftCells(tintMat_, 1);
    shiftCells(litPrev_, 1);
    {   // faces: u is (w+1) x h, v is w x (h+1); the outer faces are walls
        std::vector<float> u2(f_.u.size(), 0.0f), v2(f_.v.size(), 0.0f);
        for (int j = 0; j < h; ++j)
            for (int i = 1; i < w; ++i) {
                const int si = i + di, sj = j + dj;
                if (si >= 1 && si < w && sj >= 0 && sj < h) u2[static_cast<std::size_t>(j) * (w + 1) + i] = f_.u[static_cast<std::size_t>(sj) * (w + 1) + si];
            }
        for (int j = 1; j < h; ++j)
            for (int i = 0; i < w; ++i) {
                const int si = i + di, sj = j + dj;
                if (si >= 0 && si < w && sj >= 1 && sj < h) v2[static_cast<std::size_t>(j) * w + i] = f_.v[static_cast<std::size_t>(sj) * w + si];
            }
        f_.u.swap(u2);
        f_.v.swap(v2);
    }
    ox_ = originX;
    oy_ = originY;
    dropped -= massFixed();
    return dropped;
}

// --------------------------------------------------------------------------
// pixels -> grid

void GasGrid::buildObstacles() {
    const int w = f_.w, h = f_.h;
    const float airCap = sp_.airCap;
    std::vector<long long> absorbedRow(static_cast<std::size_t>(h), 0);
    auto row = [&](int j) {
        const int y0 = oy_ + j * kGasCell;
        for (int i = 0; i < w; ++i) {
            const int c = j * w + i;
            const int x0 = ox_ + i * kGasCell;
            Chunk* ch = chunkAt(x0, y0); // a 4x4 block never straddles a chunk
            int open = 0;
            bool flame = false, loose = false;
            if (ch) {
                for (int dy = 0; dy < kGasCell; ++dy)
                    for (int dx = 0; dx < kGasCell; ++dx) {
                        Cell& cell = ch->at((x0 + dx) & kChunkMask, (y0 + dy) & kChunkMask);
                        const Material& m = mats_[cell.material];
                        if (openKind(m.kind)) {
                            ++open;
                            if (m.kind == MaterialKind::Special) {
                                flame = true;
                                // #480: a flame pixel (brush, old save) becomes fuel vapour on the grid
                                const float fu = flameFuelOf_[cell.material];
                                if (fu > 0.0f && params_.gridFlames) {
                                    fuel_[static_cast<std::size_t>(c)] += fu;
                                    f_.heat[c] += (m.heatCapacity - airCap) * (cell.temperature - kAmbientTemp);
                                    cell.material = kEmpty;
                                    cell.flags = 0;
                                    cell.life = 0;
                                    wakePx(x0 + dx, y0 + dy);
                                    continue;
                                }
                            }
                            const int k = m.kind == MaterialKind::Gas ? speciesOf(cell.material) : -1;
                            if (k >= 0 && params_.absorbGasPixels) {
                                f_.m(k, c) += m.mass * kGasMassOne;
                                f_.heat[c] += (m.heatCapacity - airCap) * (cell.temperature - kAmbientTemp);
                                absorbedRow[j] += m.mass;
                                cell.material = kEmpty;
                                cell.flags = 0;
                                cell.life = 0;
                                wakePx(x0 + dx, y0 + dy);
                            }
                        } else {
                            if (cell.flags & kFlagBurning) flame = true;
                            if (looseKind(m.kind)) loose = true;
                        }
                    }
            }
            f_.open[c] = static_cast<float>(open) / kGasCellPx;
            flameIn_[c] = flame;
            loose_[c] = loose;
        }
    };
    if (pool_) pool_->parallelFor(h, row);
    else for (int j = 0; j < h; ++j) row(j);
    for (long long a : absorbedRow) stats_.absorbed += a;
}

// Gas (and heat) sitting in a cell that filled up moves to an open neighbour.
void GasGrid::moveTrapped() {
    const int w = f_.w, h = f_.h, ns = f_.species;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            const int c = j * w + i;
            if (f_.open[c] > 0.0f) continue;
            bool any = f_.heat[c] != 0.0f || fuel_[c] != 0.0f;
            float light = 0.0f;
            for (int k = 0; k < ns; ++k) {
                any = any || f_.m(k, c) != 0;
                light += sp_.light[k] * static_cast<float>(f_.m(k, c));
            }
            if (!any) continue;
            const int dirUp = light >= 0.0f ? -1 : 1;
            const int ti[4] = {i, i - 1, i + 1, i}, tj[4] = {j + dirUp, j, j, j - dirUp};
            for (int q = 0; q < 4; ++q) {
                if (ti[q] < 0 || tj[q] < 0 || ti[q] >= w || tj[q] >= h) continue;
                const int t = tj[q] * w + ti[q];
                if (f_.open[t] <= 0.0f) continue;
                for (int k = 0; k < ns; ++k) {
                    f_.m(k, t) += f_.m(k, c);
                    f_.m(k, c) = 0;
                }
                f_.heat[t] += f_.heat[c];
                f_.heat[c] = 0.0f;
                fuel_[t] += fuel_[c];
                soot_[t] += soot_[c];
                fuel_[c] = soot_[c] = flameT_[c] = 0.0f;
                break;
            }
        }
}

// Air pixels and their grid cell move towards a common temperature. Only
// the block mean moves (conduction smooths inside the block), and what the
// pixels gain the grid loses, so the sum is kept.
void GasGrid::exchangeHeat() {
    const int w = f_.w, h = f_.h;
    const float k = std::clamp(params_.airExchange, 0.0f, 1.0f);
    if (k <= 0.0f) return;
    const float airCap = sp_.airCap;
    auto row = [&](int j) {
        const int y0 = oy_ + j * kGasCell;
        for (int i = 0; i < w; ++i) {
            const int c = j * w + i;
            if (f_.open[c] <= 0.0f) continue;
            const int x0 = ox_ + i * kGasCell;
            Chunk* ch = chunkAt(x0, y0);
            if (!ch) continue;
            int n = 0;
            float sum = 0.0f;
            for (int dy = 0; dy < kGasCell; ++dy)
                for (int dx = 0; dx < kGasCell; ++dx) {
                    const Cell& cell = ch->at((x0 + dx) & kChunkMask, (y0 + dy) & kChunkMask);
                    if (cell.material == kEmpty) { ++n; sum += cell.temperature; }
                }
            if (n == 0) continue;
            const float ca = static_cast<float>(n) * airCap;
            const float cg = f_.capacity(c, sp_);
            const float tg = kAmbientTemp + f_.heat[c] / cg;
            const float tm = sum / static_cast<float>(n);
            const float target = (ca * tm + cg * tg) / (ca + cg);
            const float dMean = k * (target - tm);
            if (std::abs(dMean) < 0.02f) continue;
            for (int dy = 0; dy < kGasCell; ++dy)
                for (int dx = 0; dx < kGasCell; ++dx) {
                    Cell& cell = ch->at((x0 + dx) & kChunkMask, (y0 + dy) & kChunkMask);
                    if (cell.material == kEmpty) cell.temperature += dMean;
                }
            f_.heat[c] -= ca * dMean;
            ch->heatNext.store(true, std::memory_order_relaxed);
        }
    };
    if (pool_) pool_->parallelFor(h, row);
    else for (int j = 0; j < h; ++j) row(j);
}

// Flammable grid gas burns where it is hot enough or touches a flame:
// it turns into its burns_into gas (same mass), heats the cell and pushes
// outwards. Burning heats the neighbours, so a cloud goes up in a chain.
void GasGrid::burnGas() {
    const int ns = f_.species;
    const std::size_t n = f_.cells();
    const std::vector<std::uint8_t>& wasBurning = litPrev_; // a burning cell lights its neighbours next tick
    std::fill(burnt_.begin(), burnt_.end(), std::uint8_t{0});
    // #480: the glow is this tick's burning plus a fading memory of the last few
    for (std::size_t c = 0; c < n; ++c) {
        glow_[c] *= 0.5f;
        tintE_[c] *= 0.5f;
    }
    for (int k = 0; k < ns; ++k) {
        const Material& m = mats_[species_[k]];
        if (!m.flammable()) continue;
        const int into = speciesOf(m.burnsInto.into);
        if (into < 0 || into == k) continue;
        const float perFixed = static_cast<float>(m.burnHeat) * static_cast<float>(std::max(1, m.burnTime)) /
                               (static_cast<float>(m.mass) * kGasMassOne);
        for (std::size_t c = 0; c < n; ++c) {
            const std::int32_t have = f_.m(k, static_cast<int>(c));
            if (have <= 0 || f_.open[c] <= 0.0f) continue;
            const int i = static_cast<int>(c) % f_.w, j = static_cast<int>(c) / f_.w;
            const bool front = (i > 0 && wasBurning[c - 1]) || (i + 1 < f_.w && wasBurning[c + 1]) ||
                               (j > 0 && wasBurning[c - f_.w]) || (j + 1 < f_.h && wasBurning[c + f_.w]) || wasBurning[c];
            if (!flameIn_[c] && !front && temperature(static_cast<int>(c)) < m.ignitePoint) continue;
            burnt_[c] = 1;
            std::int32_t amt = static_cast<std::int32_t>(static_cast<float>(have) * params_.burnRate);
            if (have < kGasMassOne / 4 || amt <= 0) amt = have;
            f_.m(k, static_cast<int>(c)) -= amt;
            f_.m(into, static_cast<int>(c)) += amt;
            const float e = static_cast<float>(amt) * perFixed;
            f_.heat[c] += e;
            sourceHeat_ += e;
            extraSrc_[c] += params_.burnExpansion * e;
            stats_.burned += amt / kGasMassOne;
            // #480: a burning gas is a flame too (tinted by its material in the look)
            glow_[c] += e;
            tintE_[c] += e;
            tintMat_[c] = species_[k];
            const float tad = (flameTempOf_[static_cast<std::size_t>(k)] > 0.0f ? flameTempOf_[static_cast<std::size_t>(k)] : params_.flameTemp) - kAmbientTemp;
            if (tad > flameT_[c]) flameT_[c] += (tad - flameT_[c]) * (e / (e + 4.0f));
            flameFields_ = true;
        }
    }
}

// #480: fuel vapour from burning pixels (and blasts, and flame pixels) burns
// on the grid. A cell burns when it is lit: a flame or burning pixel in its
// block, a cell next to it burnt last tick (the flame front), gas burning in
// it now, or hot enough. Smoke and other inert gas in the block slow it down
// (less air), oxygen speeds it up. The energy becomes heat of the cell
// (counted in sourceHeat) and expansion, plus soot; the burning gas itself
// is drawn at its flame temperature (flameT_, sub-cell, look and light only).
// Unlit fuel cools away. Serial and in cell order: deterministic.
void GasGrid::burnFuel() {
    const int w = f_.w, h = f_.h, ns = f_.species;
    stats_.flameCells = 0;
    stats_.fuelBurned = 0;
    stats_.maxTemp = 0;
    double left = 0;
    bool any = false;
    const float rate = std::clamp(params_.fuelBurnRate, 0.0f, 1.0f);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            const int c = j * w + i;
            const std::size_t cs = static_cast<std::size_t>(c);
            if (glow_[cs] > 0.5f) stats_.maxTemp = std::max(stats_.maxTemp, flameTemperature(c));
            float& fu = fuel_[cs];
            if (fu <= 0.0f) {
                any = any || flameT_[cs] > 0.0f || soot_[cs] > 0.0f || glow_[cs] > 0.0f;
                continue;
            }
            any = true;
            if (f_.open[cs] <= 0.0f) { left += fu; continue; }
            const bool front = litPrev_[cs] || (i > 0 && litPrev_[cs - 1]) || (i + 1 < w && litPrev_[cs + 1]) ||
                               (j > 0 && litPrev_[cs - w]) || (j + 1 < h && litPrev_[cs + w]);
            if (!flameIn_[cs] && !front && !burnt_[cs] && temperature(c) < params_.fuelIgnite) {
                const float d = fu < 0.05f ? fu : fu * params_.fuelDecay;
                fu -= d;
                soot_[cs] += d * params_.sootYield;
                left += fu;
                continue;
            }
            float inert = 0.0f, o2 = 0.0f;
            for (int k = 0; k < ns; ++k) (k == oxygen_ ? o2 : inert) += static_cast<float>(f_.m(k, c));
            const float per = 1.0f / (f_.open[cs] * kGasCellPx * kGasMassOne);
            const float air = std::clamp(1.0f - 0.5f * inert * per, 0.15f, 1.0f);
            const float boost = std::min(1.0f + 2.0f * o2 * per, 3.0f);
            float b = fu * std::min(1.0f, rate * air * boost);
            if (fu - b < 0.05f) b = fu;
            fu -= b;
            left += fu;
            f_.heat[cs] += b;
            sourceHeat_ += b;
            extraSrc_[cs] += params_.burnExpansion * b;
            glow_[cs] += b;
            soot_[cs] += b * params_.sootYield * (1.0f + 1.5f * (1.0f - air));
            const float tad = params_.flameTemp * (o2 * per > 0.05f ? 1.25f : 1.0f) - kAmbientTemp;
            if (tad > flameT_[cs]) flameT_[cs] += (tad - flameT_[cs]) * (b / (b + 4.0f));
            burnt_[cs] = 1;
            ++stats_.flameCells;
            stats_.fuelBurned += b;
        }
    stats_.fuel = left;
    flameFields_ = any;
}

// #480: fuel, soot and the flame heat ride the solved velocity
// (semi-Lagrangian, bilinear over open cells), so a flame is carried up and
// swirled by the flow; soot thins out and the flame heat cools as it goes.
// Each cell reads only the previous buffers: rows in parallel, same result
// for any thread count. Not mass-exact (fuel isn't mass; what burns is
// counted when it burns).
void GasGrid::advectFlame() {
    if (!flameFields_) return;
    const int w = f_.w, h = f_.h;
    const std::size_t n = f_.cells();
    fuel2_.resize(n);
    soot2_.resize(n);
    flameT2_.resize(n);
    const float sKeep = 1.0f - std::clamp(params_.sootDecay, 0.0f, 1.0f);
    const float tKeep = 1.0f - std::clamp(params_.flameCool, 0.0f, 1.0f);
    auto row = [&](int j) {
        for (int i = 0; i < w; ++i) {
            const std::size_t c = static_cast<std::size_t>(j) * w + i;
            if (f_.open[c] <= 0.0f) {
                fuel2_[c] = fuel_[c];
                soot2_[c] = 0.0f;
                flameT2_[c] = 0.0f;
                continue;
            }
            const float x = static_cast<float>(i) - f_.uc(i, j), y = static_cast<float>(j) - f_.vc(i, j);
            const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
            const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
            float ws = 0.0f, a = 0.0f, s = 0.0f, t = 0.0f;
            for (int q = 0; q < 4; ++q) {
                const int ci = x0 + (q & 1), cj = y0 + (q >> 1);
                if (ci < 0 || cj < 0 || ci >= w || cj >= h) continue;
                const std::size_t k = static_cast<std::size_t>(cj) * w + ci;
                if (f_.open[k] <= 0.0f) continue;
                const float wq = ((q & 1) ? fx : 1.0f - fx) * ((q >> 1) ? fy : 1.0f - fy);
                ws += wq;
                a += wq * fuel_[k];
                s += wq * soot_[k];
                t += wq * flameT_[k];
            }
            if (ws > 1e-4f) {
                a /= ws;
                s /= ws;
                t /= ws;
            } else {
                a = fuel_[c];
                s = soot_[c];
                t = flameT_[c];
            }
            s *= sKeep;
            t *= tKeep;
            fuel2_[c] = a < 1e-3f ? 0.0f : a;
            soot2_[c] = s < 1e-4f ? 0.0f : s;
            flameT2_[c] = t < 1.0f ? 0.0f : t;
        }
    };
    if (pool_) pool_->parallelFor(h, row);
    else for (int j = 0; j < h; ++j) row(j);
    fuel_.swap(fuel2_);
    soot_.swap(soot2_);
    flameT_.swap(flameT2_);
}

// A cool enough gas falls back out as pixels (steam -> water, smoke -> ash),
// one pixel per mass unit, into air pixels of its block. The pixel gets the
// unit's heat, the heat of the air pixel it replaces and the latent heat.
void GasGrid::condense() {
    const int w = f_.w, h = f_.h, ns = f_.species;
    const float airCap = sp_.airCap;
    for (int k = 0; k < ns; ++k) {
        const Material& m = mats_[species_[k]];
        if (!m.cold.valid || speciesOf(m.cold.into) >= 0) continue;
        const Material& into = mats_[m.cold.into];
        const std::int32_t unit = m.mass * kGasMassOne;
        const float unitCap = sp_.cap[k] * static_cast<float>(unit) / kGasMassOne; // = heat_capacity
        const float latent = (into.hot.valid && into.hot.into == m.id) ? into.latentHeat : 0.0f;
        const auto chance = static_cast<std::uint32_t>(std::clamp(m.changeChance, 0.0f, 1.0f) * 65536.0f);
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) {
                const int c = j * w + i;
                if (f_.open[c] <= 0.0f || f_.m(k, c) < unit) continue;
                const float tg = temperature(c);
                if (tg > m.cold.point) continue;
                Rng rng = cellRng(c, 1 + static_cast<std::uint64_t>(k));
                const int x0 = ox_ + i * kGasCell, y0 = oy_ + j * kGasCell;
                for (int tries = 0; tries < 2 && f_.m(k, c) >= unit; ++tries) {
                    if (!rng.chance16(chance)) continue;
                    const int x = x0 + static_cast<int>(rng.below(kGasCell)), y = y0 + static_cast<int>(rng.below(kGasCell));
                    Cell* cell = px(x, y);
                    if (!cell || cell->material != kEmpty) continue;
                    const float eGas = unitCap * (tg - kAmbientTemp);
                    const float eAir = airCap * (cell->temperature - kAmbientTemp);
                    Cell nc = into.spawn(static_cast<std::uint8_t>(rng.below(256)));
                    nc.temperature = std::clamp(kAmbientTemp + (eGas + eAir + latent) / into.heatCapacity, kMinTemp, kMaxTemp);
                    *cell = nc;
                    wakePx(x, y);
                    f_.m(k, c) -= unit;
                    f_.heat[c] -= eGas;
                    latentHeat_ += latent;
                    ++stats_.condensed;
                }
            }
    }
}

// Expansion source: crowded cells push out towards emptier ones (sums to
// zero over a closed region), plus explosions and burning.
void GasGrid::buildSources() {
    const int w = f_.w, h = f_.h, ns = f_.species;
    std::vector<float> conc(f_.cells(), 0.0f);
    for (int c = 0; c < static_cast<int>(f_.cells()); ++c) {
        if (f_.open[c] <= 0.0f) continue;
        float s = 0.0f;
        for (int k = 0; k < ns; ++k) s += static_cast<float>(f_.m(k, c));
        conc[c] = s / (f_.open[c] * kGasCellPx * kGasMassOne);
    }
    const float g = params_.crowding;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            const int c = j * w + i;
            float s = extraSrc_[c];
            extraSrc_[c] = 0.0f;
            if (f_.open[c] > 0.0f && g > 0.0f) {
                if (i > 0 && f_.open[c - 1] > 0.0f) s += g * (conc[c] - conc[c - 1]);
                if (i + 1 < w && f_.open[c + 1] > 0.0f) s += g * (conc[c] - conc[c + 1]);
                if (j > 0 && f_.open[c - w] > 0.0f) s += g * (conc[c] - conc[c - w]);
                if (j + 1 < h && f_.open[c + w] > 0.0f) s += g * (conc[c] - conc[c + w]);
            }
            f_.src[c] = f_.open[c] > 0.0f ? std::clamp(s, -1.5f, 1.5f) : 0.0f;
        }
}

// Fast gas moves the liquid and powder pixels it blows over or against,
// one pixel step at a time, with a chance that grows with the speed.
void GasGrid::pushPixels() {
    const int w = f_.w, h = f_.h;
    const float thr = params_.pushThreshold;
    stats_.pushed = 0;
    if (params_.push <= 0.0f) return;
    // A row of cells touches its own block row, one block row up or down and
    // one pixel past that, so rows four apart never share a pixel: four
    // phases of parallel rows, same result for any thread count.
    std::vector<int> pushedRow(static_cast<std::size_t>(h), 0);
    auto row = [&](int j) {
        for (int i = 0; i < w; ++i) {
            const int c = j * w + i;
            if (f_.open[c] <= 0.0f) continue;
            const float ux = f_.uc(i, j), vy = f_.vc(i, j);
            const float sp = std::sqrt(ux * ux + vy * vy);
            if (sp <= thr) continue;
            const bool horizontal = std::abs(ux) >= std::abs(vy);
            const int dx = horizontal ? (ux > 0 ? 1 : -1) : 0, dy = horizontal ? 0 : (vy > 0 ? 1 : -1);
            const int ni = i + dx, nj = j + dy;
            const bool aheadLoose = ni >= 0 && nj >= 0 && ni < w && nj < h && loose_[nj * w + ni];
            if (!loose_[c] && !aheadLoose) continue;
            Rng rng = cellRng(c, 7);
            const float chance = std::min(1.0f, (sp - thr) * params_.push);
            // the block and the one ahead of it, scanned from the far side so a pixel moves once
            const int x0 = ox_ + std::min(i, ni) * kGasCell, y0 = oy_ + std::min(j, nj) * kGasCell;
            const int spanX = (dx != 0 ? 2 : 1) * kGasCell, spanY = (dy != 0 ? 2 : 1) * kGasCell;
            for (int a = 0; a < spanY; ++a)
                for (int b = 0; b < spanX; ++b) {
                    const int x = dx > 0 ? x0 + spanX - 1 - b : x0 + b;
                    const int y = dy > 0 ? y0 + spanY - 1 - a : y0 + a;
                    Cell* p = px(x, y);
                    if (!p || !looseKind(mats_[p->material].kind)) continue;
                    Cell* t = px(x + dx, y + dy);
                    if (!t || !openKind(mats_[t->material].kind) || t->material != kEmpty) continue;
                    if (rng.unit() >= chance) continue;
                    std::swap(*p, *t);
                    t->vy = 0;
                    wakePx(x, y);
                    wakePx(x + dx, y + dy);
                    ++pushedRow[static_cast<std::size_t>(j)];
                }
        }
    };
    for (int phase = 0; phase < 4; ++phase) {
        const int count = (h - phase + 3) / 4;
        if (count <= 0) continue;
        if (pool_) pool_->parallelFor(count, [&](int r) { row(phase + 4 * r); });
        else for (int r = 0; r < count; ++r) row(phase + 4 * r);
    }
    for (int n : pushedRow) stats_.pushed += n;
}

void GasGrid::applyBlast(const Blast& b) {
    const int x = b.x, y = b.y, radius = b.radius;
    const float power = b.power;
    const int w = f_.w, h = f_.h;
    const float rc = static_cast<float>(radius) / kGasCell + 1.0f;
    const float cx = static_cast<float>(x - ox_) / kGasCell, cy = static_cast<float>(y - oy_) / kGasCell;
    for (int j = std::max(0, static_cast<int>(cy - rc)); j <= std::min(h - 1, static_cast<int>(cy + rc)); ++j)
        for (int i = std::max(0, static_cast<int>(cx - rc)); i <= std::min(w - 1, static_cast<int>(cx + rc)); ++i) {
            const int c = j * w + i;
            const float ddx = static_cast<float>(i) + 0.5f - cx, ddy = static_cast<float>(j) + 0.5f - cy;
            const float d = std::sqrt(ddx * ddx + ddy * ddy);
            if (d > rc || f_.open[c] <= 0.0f) continue;
            const float fall = 1.0f - d / rc;
            extraSrc_[c] += 0.6f * power * fall;
            const float e = 30.0f * power * fall * f_.open[c];
            f_.heat[c] += e;
            sourceHeat_ += e;
            if (params_.gridFlames && params_.blastFuel > 0.0f) {
                // #480: the fireball: fuel that burns over the next ticks, carried by the blast
                fuel_[static_cast<std::size_t>(c)] += params_.blastFuel * power * fall * f_.open[c];
                litPrev_[static_cast<std::size_t>(c)] = 1;
                flameFields_ = true;
            }
            // a radial kick on the cell's faces
            const float nx = d > 0.01f ? ddx / d : 0.0f, ny = d > 0.01f ? ddy / d : 0.0f;
            const float kick = std::min(1.5f, 0.8f * power * fall);
            f_.u[j * (w + 1) + i + (nx > 0 ? 1 : 0)] += kick * nx;
            f_.v[(j + (ny > 0 ? 1 : 0)) * w + i] += kick * ny;
        }
}

// Gas pressure on pixels. The solver is incompressible, so gas that piles
// up in a pocket (boiling under water, a sealed room filling with smoke)
// shows up as concentration. Above pocketPressure units per open pixel the
// pocket pushes: a line of liquid or powder next to it moves one pixel
// outwards, into the first air pixel along that line, and the pocket gains
// that pixel. Light pockets push up first, heavy ones down. Serial: a line
// can be long and cross block rows.
void GasGrid::expandPockets() {
    stats_.shoved = 0;
    const float thr = params_.pocketPressure;
    if (thr <= 0.0f || params_.pocketReach <= 0) return;
    const int w = f_.w, h = f_.h, ns = f_.species;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            const int c = j * w + i;
            if (f_.open[c] <= 0.0f) continue;
            const bool nearLoose = loose_[c] || (i > 0 && loose_[c - 1]) || (i + 1 < w && loose_[c + 1]) ||
                                   (j > 0 && loose_[c - w]) || (j + 1 < h && loose_[c + w]);
            if (!nearLoose) continue;
            float total = 0.0f, light = 0.0f;
            for (int k = 0; k < ns; ++k) {
                total += static_cast<float>(f_.m(k, c));
                light += sp_.light[k] * static_cast<float>(f_.m(k, c));
            }
            const float conc = total / (f_.open[c] * kGasCellPx * kGasMassOne);
            if (conc <= thr) continue;
            Rng rng = cellRng(c, 11);
            const int tries = std::min(6, static_cast<int>((conc - thr) * 6.0f) + 1);
            // push towards thinner gas: the weight of a side is the concentration
            // drop to that neighbour (a filled block counts as 0), a little more
            // along buoyancy
            const int up = light >= 0.0f ? -1 : 1;
            static constexpr int kDx[4] = {1, -1, 0, 0}, kDy[4] = {0, 0, -1, 1};
            float wgt[4] = {0, 0, 0, 0}, sum = 0.0f;
            for (int d = 0; d < 4; ++d) {
                const int ni = i + kDx[d], nj = j + kDy[d];
                if (ni < 0 || nj < 0 || ni >= w || nj >= h) continue;
                const int nc = nj * w + ni;
                if (!loose_[nc] && !loose_[c]) continue;
                float nconc = 0.0f;
                if (f_.open[nc] > 0.0f) {
                    for (int k = 0; k < ns; ++k) nconc += static_cast<float>(f_.m(k, nc));
                    nconc /= f_.open[nc] * kGasCellPx * kGasMassOne;
                }
                wgt[d] = std::max(0.0f, conc - nconc) * (kDy[d] == up ? 1.5f : 1.0f);
                sum += wgt[d];
            }
            if (sum <= 0.0f) continue;
            for (int t = 0; t < tries; ++t) {
                float pick = rng.unit() * sum;
                int dir = 0;
                while (dir < 3 && pick >= wgt[dir]) pick -= wgt[dir++];
                const int dx = kDx[dir], dy = kDy[dir];
                // an air pixel of the pocket with a loose pixel right next to it in that direction
                // (the block's 16 pixels from a random start)
                const int start = static_cast<int>(rng.below(kGasCellPx));
                int sx = 0, sy = 0;
                Cell* l = nullptr;
                for (int q = 0; q < kGasCellPx && !l; ++q) {
                    const int idx = (start + q) % kGasCellPx;
                    sx = ox_ + i * kGasCell + idx % kGasCell;
                    sy = oy_ + j * kGasCell + idx / kGasCell;
                    Cell* s = px(sx, sy);
                    Cell* nb = px(sx + dx, sy + dy);
                    if (s && nb && s->material == kEmpty && looseKind(mats_[nb->material].kind)) l = nb;
                }
                if (!l) continue;
                // the first air pixel past the line of loose pixels
                int n = 1;
                Cell* e = nullptr;
                for (; n <= params_.pocketReach; ++n) {
                    Cell* q = px(sx + dx * (n + 1), sy + dy * (n + 1));
                    if (!q) break;
                    if (q->material == kEmpty) { e = q; break; }
                    if (!looseKind(mats_[q->material].kind)) break;
                }
                if (!e) continue;
                // shift the line one pixel outwards; the far air pixel ends up next to the pocket
                const Cell air = *e;
                for (int k = n + 1; k > 1; --k) {
                    *px(sx + dx * k, sy + dy * k) = *px(sx + dx * (k - 1), sy + dy * (k - 1));
                    wakePx(sx + dx * k, sy + dy * k);
                }
                *l = air;
                wakePx(sx + dx, sy + dy);
                ++stats_.shoved;
            }
        }
}

void GasGrid::jet(int x, int y, MaterialId gas, int mass, float temperature, float vx, float vy) {
    if (f_.w <= 0) return;
    if (mass > 0 && speciesOf(gas) >= 0) {
        GasEmission e;
        e.x = x;
        e.y = y;
        e.material = gas;
        e.mass = mass;
        e.temperature = temperature;
        absorb(&e, 1);
    }
    kicks_.push_back({x, y, vx, vy});
}

void GasGrid::applyKick(const Kick& k) {
    const int w = f_.w, h = f_.h;
    const float cu = std::clamp(k.vx / kGasCell, -sp_.maxSpeed, sp_.maxSpeed);
    const float cv = std::clamp(k.vy / kGasCell, -sp_.maxSpeed, sp_.maxSpeed);
    const float len = std::sqrt(k.vx * k.vx + k.vy * k.vy);
    const float sx = len > 0.0f ? k.vx / len : 0.0f, sy = len > 0.0f ? k.vy / len : 0.0f;
    for (int s = 0; s < 3; ++s) {
        // the nozzle cell and the next two along the jet
        const int i = (k.x + static_cast<int>(std::lround(sx * kGasCell * s)) - ox_) / kGasCell;
        const int j = (k.y + static_cast<int>(std::lround(sy * kGasCell * s)) - oy_) / kGasCell;
        if (i < 0 || j < 0 || i >= w || j >= h || f_.open[j * w + i] <= 0.0f) continue;
        const float keep = 0.35f; // blend the faces towards the jet speed
        float& ul = f_.u[j * (w + 1) + i];
        float& ur = f_.u[j * (w + 1) + i + 1];
        float& vt = f_.v[j * w + i];
        float& vb = f_.v[(j + 1) * w + i];
        ul = ul * keep + cu * (1.0f - keep);
        ur = ur * keep + cu * (1.0f - keep);
        vt = vt * keep + cv * (1.0f - keep);
        vb = vb * keep + cv * (1.0f - keep);
    }
}

void GasGrid::explode(int x, int y, int radius, float power) {
    if (f_.w <= 0 || radius <= 0 || power <= 0.0f) return;
    blasts_.push_back({x, y, radius, power});
    buildChunkTable();
    // throw the loose pixels in the radius outwards (outer ones first, so they make room)
    struct Px { int x, y; float d; };
    std::vector<Px> loose;
    for (int qy = y - radius; qy <= y + radius; ++qy)
        for (int qx = x - radius; qx <= x + radius; ++qx) {
            const float d = std::sqrt(static_cast<float>((qx - x) * (qx - x) + (qy - y) * (qy - y)));
            if (d > static_cast<float>(radius)) continue;
            const Cell* c = px(qx, qy);
            if (c && looseKind(mats_[c->material].kind)) loose.push_back({qx, qy, d});
        }
    std::stable_sort(loose.begin(), loose.end(), [](const Px& a, const Px& b) { return a.d > b.d; });
    if (particles_) {
        // phase 4: the debris flies as particles and lands wherever it stops
        Rng rng = cellRng(x * 7919 + y, 13);
        for (const Px& p : loose) {
            const float d = std::max(p.d, 0.5f);
            float nx = static_cast<float>(p.x - x) / d, ny = static_cast<float>(p.y - y) / d - 0.5f;
            const float len = std::sqrt(nx * nx + ny * ny);
            nx /= len;
            ny /= len;
            const float sp = blastSpeed * power * (1.0f - p.d / (static_cast<float>(radius) + 1.0f)) * (0.6f + 0.6f * rng.unit());
            if (sp < 0.5f) continue;
            particles_->launch(p.x, p.y, nx * sp, ny * sp);
        }
        return;
    }
    for (const Px& p : loose) {
        const float d = std::max(p.d, 0.5f);
        // outwards with a lift, so a crater opens and the debris flies up
        float nx = static_cast<float>(p.x - x) / d, ny = static_cast<float>(p.y - y) / d - 0.5f;
        const float len = std::sqrt(nx * nx + ny * ny);
        nx /= len;
        ny /= len;
        const float reach = power * static_cast<float>(radius) * (1.0f - p.d / (static_cast<float>(radius) + 1.0f));
        const int steps = std::clamp(static_cast<int>(reach) + 2, 2, 48);
        // land on the farthest air pixel along the ray, flying over loose stuff, stopped by solids
        int bx = p.x, by = p.y;
        for (int s = 1; s <= steps; ++s) {
            const int tx = p.x + static_cast<int>(std::lround(nx * static_cast<float>(s)));
            const int ty = p.y + static_cast<int>(std::lround(ny * static_cast<float>(s)));
            const Cell* t = px(tx, ty);
            if (!t) break;
            const MaterialKind k = mats_[t->material].kind;
            if (k == MaterialKind::Solid) break;
            if (t->material == kEmpty) { bx = tx; by = ty; }
        }
        if (bx == p.x && by == p.y) continue;
        Cell* a = px(p.x, p.y);
        Cell* b = px(bx, by);
        std::swap(*a, *b);
        wakePx(p.x, p.y);
        wakePx(bx, by);
    }
}

void GasGrid::step() {
    if (!params_.enabled || f_.w <= 0) return;
    const auto t0 = std::chrono::steady_clock::now();
    ++tick_;
    buildChunkTable();
    stats_.absorbed = absorbedPending_;
    absorbedPending_ = 0;
    stats_.condensed = 0;
    stats_.burned = 0;
    buildObstacles();
    stats_.obstaclesMs = msSince(t0);
    for (const Blast& b : blasts_) applyBlast(b);
    blasts_.clear();
    for (const Kick& k : kicks_) applyKick(k);
    kicks_.clear();
    moveTrapped();
    exchangeHeat();
    burnGas();
    burnFuel();
    litPrev_.swap(burnt_); // this tick's burning cells light their neighbours next tick
    condense();
    buildSources();
    if (params_.ambientLoss > 0.0f) {
        const float keep = 1.0f - std::clamp(params_.ambientLoss, 0.0f, 1.0f);
        double lost = 0.0;
        for (float& hv : f_.heat) {
            const float nv = hv * keep;
            lost += static_cast<double>(hv) - nv;
            hv = nv;
        }
        lostHeat_ += lost;
    }
    const auto t1 = std::chrono::steady_clock::now();
    solver_->solve(f_, sp_);
    stats_.solveMs = msSince(t1);
    advectFlame();
    // with walls all round, pressure is only defined up to a constant: keep its mean at 0
    {
        double sum = 0;
        int open = 0;
        for (std::size_t c = 0; c < f_.cells(); ++c)
            if (f_.open[c] > 0.0f) { sum += f_.p[c]; ++open; }
        const float mean = open > 0 ? static_cast<float>(sum / open) : 0.0f;
        for (std::size_t c = 0; c < f_.cells(); ++c)
            if (f_.open[c] > 0.0f) f_.p[c] -= mean;
    }
    const auto t2 = std::chrono::steady_clock::now();
    pushPixels();
    expandPockets();
    stats_.pushMs = msSince(t2);
    float ms = 0, mp = 0;
    for (int j = 0; j < f_.h; ++j)
        for (int i = 0; i < f_.w; ++i) {
            ms = std::max(ms, f_.speedAt(i, j));
            mp = std::max(mp, std::abs(f_.p[j * f_.w + i]));
        }
    stats_.maxSpeed = ms;
    stats_.maxPressure = mp;
    stats_.coupleMs = msSince(t0) - stats_.solveMs;
}

} // namespace notita
