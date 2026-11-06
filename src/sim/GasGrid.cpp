#include "GasGrid.h"

#include <algorithm>
#include <chrono>
#include <cmath>

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
        const int k = speciesOf(e[q].material);
        if (k < 0) continue;
        const int i = std::clamp((e[q].x - ox_) / kGasCell, 0, f_.w - 1);
        const int j = std::clamp((e[q].y - oy_) / kGasCell, 0, f_.h - 1);
        const int c = j * f_.w + i;
        f_.m(k, c) += e[q].mass * kGasMassOne;
        // the pixel became air at the same temperature; the grid takes the rest of the gas's heat
        f_.heat[c] += (mats_[e[q].material].heatCapacity - sp_.airCap) * (e[q].temperature - kAmbientTemp);
        absorbedPending_ += e[q].mass;
    }
}

void GasGrid::clear() {
    lostHeat_ += heatEnergy();
    std::fill(f_.u.begin(), f_.u.end(), 0.0f);
    std::fill(f_.v.begin(), f_.v.end(), 0.0f);
    std::fill(f_.p.begin(), f_.p.end(), 0.0f);
    std::fill(f_.mass.begin(), f_.mass.end(), 0);
    std::fill(f_.heat.begin(), f_.heat.end(), 0.0f);
    std::fill(extraSrc_.begin(), extraSrc_.end(), 0.0f);
    std::fill(burnt_.begin(), burnt_.end(), std::uint8_t{0});
    blasts_.clear();
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
                            if (m.kind == MaterialKind::Special) flame = true;
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
            bool any = f_.heat[c] != 0.0f;
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
    const std::vector<std::uint8_t> wasBurning = burnt_; // a burning cell lights its neighbours next tick
    std::fill(burnt_.begin(), burnt_.end(), std::uint8_t{0});
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
        }
    }
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
    moveTrapped();
    exchangeHeat();
    burnGas();
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
