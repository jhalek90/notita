#include "Electric.h"

#include "ChunkCodec.h"
#include "Player.h"
#include "Rng.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace notita {

namespace {

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t");
    const auto e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

const std::string* extraOf(const Material& m, const char* key) {
    auto it = m.extra.find(key);
    return it == m.extra.end() ? nullptr : &it->second;
}

float floatOf(const Material& m, const char* key, float def) {
    const std::string* s = extraOf(m, key);
    return s ? std::strtof(s->c_str(), nullptr) : def;
}

constexpr int kDx[4] = {1, -1, 0, 0};
constexpr int kDy[4] = {0, 0, 1, -1};

} // namespace

const char* electricRoleName(ElectricRole r) {
    switch (r) {
    case ElectricRole::None: return "none";
    case ElectricRole::Source: return "source";
    case ElectricRole::Drain: return "drain";
    case ElectricRole::Thermo: return "thermopile";
    case ElectricRole::Lamp: return "lamp";
    case ElectricRole::LampLit: return "lamp_lit";
    case ElectricRole::Plate: return "plate";
    case ElectricRole::Switch: return "switch";
    }
    return "?";
}

ElectricSystem::ElectricSystem(World& world, const MaterialRegistry& mats, ThreadPool* pool, std::uint64_t seed)
    : world_(world), mats_(mats), pool_(pool), seed_(seed) {
    props_.resize(mats.size());
    for (const Material& m : mats.all()) {
        ElectricProps& p = props_[m.id];
        if (m.id == kEmpty) continue;
        p.conduct = std::clamp(floatOf(m, "electric", 0.0f), 0.0f, 1.0f);
        p.heatShare = std::clamp(floatOf(m, "electric_heat", 1.0f), 0.0f, 1.0f);
        if (const std::string* r = extraOf(m, "electric_role")) {
            const std::string role = trim(*r);
            if (role == "source") p.role = ElectricRole::Source;
            else if (role == "drain") p.role = ElectricRole::Drain;
            else if (role == "thermopile") p.role = ElectricRole::Thermo;
            else if (role == "lamp") p.role = ElectricRole::Lamp;
            else if (role == "lamp_lit") p.role = ElectricRole::LampLit;
            else if (role == "plate") p.role = ElectricRole::Plate;
            else if (role == "switch") p.role = ElectricRole::Switch;
            else errors_.push_back(m.name + ": unknown electric_role '" + role + "'");
        }
        p.voltage = floatOf(m, "voltage", p.role == ElectricRole::Thermo ? 0.1f : 100.0f);
        if (p.fixed() && p.conduct <= 0.0f) p.conduct = 1.0f; // a terminal always connects
        if (const std::string* t = extraOf(m, "toggles_into")) {
            const MaterialId into = mats.idOf(trim(*t));
            if (into == kEmpty) errors_.push_back(m.name + ": toggles_into unknown material '" + trim(*t) + "'");
            else if (mats[into].mass != m.mass || mats[into].kind != m.kind)
                errors_.push_back(m.name + ": toggles_into '" + mats[into].name + "' must have the same kind and mass");
            else p.toggle = into;
        }
        if ((p.role == ElectricRole::Lamp || p.role == ElectricRole::LampLit || p.role == ElectricRole::Switch) &&
            p.toggle == kEmpty && !extraOf(m, "toggles_into"))
            errors_.push_back(m.name + ": " + electricRoleName(p.role) + " needs toggles_into");
        if (const std::string* s = extraOf(m, "splits_into")) {
            const auto comma = s->find(',');
            const std::string a = trim(s->substr(0, comma)), b = comma == std::string::npos ? "" : trim(s->substr(comma + 1));
            const MaterialId ia = mats.idOf(a), ib = mats.idOf(b);
            if (ia == kEmpty || ib == kEmpty) errors_.push_back(m.name + ": splits_into needs two known materials ('" + *s + "')");
            else if (mats[ia].mass + mats[ib].mass != 2 * m.mass)
                errors_.push_back(m.name + ": splits_into " + a + " + " + b + " doesn't keep the mass of two " + m.name);
            else { p.splitA = ia; p.splitB = ib; }
            p.splitCurrent = std::max(1e-4f, floatOf(m, "split_current", 0.05f));
        }
    }
    serialRole_.assign(mats.size(), 0);
    for (std::size_t m = 0; m < props_.size(); ++m) {
        const ElectricProps& p = props_[m];
        serialRole_[m] = p.role == ElectricRole::Lamp || p.role == ElectricRole::LampLit || p.splitA != kEmpty ? 1 : 0;
    }
    spark_ = mats.idOf("spark");
    if (spark_ != kEmpty && mats[spark_].kind != MaterialKind::Special) {
        errors_.push_back("spark must be a special (flame-kind) material");
        spark_ = kEmpty;
    }
}

// --------------------------------------------------------------------------
// grid over the loaded chunks

void ElectricSystem::buildGrid() {
    const auto& chunks = world_.chunks();
    if (chunks.empty()) { cw_ = ch_ = gw_ = gh_ = 0; return; }
    gx0_ = world_.minX();
    gy0_ = world_.minY();
    cw_ = world_.widthPx() >> kChunkShift;
    ch_ = world_.heightPx() >> kChunkShift;
    gw_ = cw_ * kChunkSize;
    gh_ = ch_ * kChunkSize;
    grid_.assign(static_cast<std::size_t>(cw_) * ch_, nullptr);
    const int bcx = gx0_ >> kChunkShift, bcy = gy0_ >> kChunkShift;
    for (Chunk* c : chunks) grid_[static_cast<std::size_t>(c->coord.y - bcy) * cw_ + (c->coord.x - bcx)] = c;
    const std::size_t n = static_cast<std::size_t>(gw_) * gh_;
    if (nodeOf_.size() != n) nodeOf_.assign(n, -1);
    if (cond_.size() != n) cond_.assign(n, 0);
    if (mark_.size() != n) { mark_.assign(n, 0); markStamp_ = 0; }
}

Cell* ElectricSystem::gridCell(int gx, int gy) const {
    if (gx < 0 || gy < 0 || gx >= gw_ || gy >= gh_) return nullptr;
    Chunk* c = grid_[static_cast<std::size_t>(gy >> kChunkShift) * cw_ + (gx >> kChunkShift)];
    return c ? &c->at(gx & kChunkMask, gy & kChunkMask) : nullptr;
}

bool ElectricSystem::loaded(int gx, int gy) const {
    // the first pixel above the plate (a thick plate's own upper rows don't count)
    const MaterialId plate = gridCell(gx, gy)->material;
    int y = gy - 1;
    const Cell* above = gridCell(gx, y);
    for (int k = 0; above && above->material == plate && !(above->flags & kFlagBody) && k < 8; ++k) above = gridCell(gx, --y);
    if (above) {
        const MaterialKind k = mats_[above->material].kind;
        if (above->material != plate && (k == MaterialKind::Solid || k == MaterialKind::Powder || k == MaterialKind::Liquid)) return true;
    }
    return player_ && player_->covers(gx + gx0_, y + gy0_);
}

bool ElectricSystem::conductsAt(int gx, int gy) const {
    const Cell* c = gridCell(gx, gy);
    if (!c || (c->flags & kFlagBody)) return false;
    const ElectricProps& p = props_[c->material];
    if (p.conduct <= 0.0f) return false;
    return p.role != ElectricRole::Plate || loaded(gx, gy);
}

ElectricSystem::Field& ElectricSystem::fieldAt(int gx, int gy) {
    const int cx = (gx + gx0_) >> kChunkShift, cy = (gy + gy0_) >> kChunkShift;
    return fields_[chunkKey(cx, cy)];
}

void ElectricSystem::wakeAt(int gx, int gy, bool motion) {
    const int x = gx + gx0_, y = gy + gy0_;
    if (motion) { world_.wake(x, y); return; }
    // heat only: the chunks the pixel's conduction pairs belong to (its own, left and upper)
    for (int dy = -1; dy <= 0; ++dy)
        for (int dx = -1; dx <= 0; ++dx) {
            const int px = gx + dx, py = gy + dy;
            if (px < 0 || py < 0) continue;
            Chunk* c = grid_[static_cast<std::size_t>(py >> kChunkShift) * cw_ + (px >> kChunkShift)];
            if (c) c->heatNext.store(true, std::memory_order_relaxed);
        }
}

void ElectricSystem::heatCell(int gx, int gy, Cell& c, float energy) {
    const float dT = std::min(energy / mats_[c.material].heatCapacity, params_.maxHeat);
    if (dT <= 1e-5f) return;
    const float before = c.temperature;
    c.temperature = std::clamp(c.temperature + dT, kMinTemp, kMaxTemp);
    stats_.heat += static_cast<double>(c.temperature - before) * mats_[c.material].heatCapacity;
    wakeAt(gx, gy, false);
}

// --------------------------------------------------------------------------
// the step

namespace {

// Node loops run over fixed blocks of nodes; partial sums are added in block
// order, so the result is the same with or without the pool (and for any
// thread count).
constexpr std::size_t kBlock = 4096;
// Components at least this big are solved one at a time with the blocks in
// parallel; smaller ones are solved side by side, each on one thread.
constexpr std::size_t kBigComponent = 3 * kBlock;

struct SumMax {
    double sum = 0.0, max = 0.0, sum2 = 0.0, sum3 = 0.0;
};

template <class F>
SumMax overBlocks(ThreadPool* pool, std::size_t m, F&& f) {
    const int nb = static_cast<int>((m + kBlock - 1) / kBlock);
    if (nb <= 1) return m ? f(std::size_t{0}, m) : SumMax{};
    std::vector<SumMax> part(static_cast<std::size_t>(nb));
    auto job = [&](int k) {
        const std::size_t k0 = static_cast<std::size_t>(k) * kBlock;
        part[static_cast<std::size_t>(k)] = f(k0, std::min(m, k0 + kBlock));
    };
    if (pool) pool->parallelFor(nb, job);
    else for (int k = 0; k < nb; ++k) job(k);
    SumMax s;
    for (const SumMax& p : part) { s.sum += p.sum; s.max = std::max(s.max, p.max); s.sum2 += p.sum2; s.sum3 += p.sum3; }
    return s;
}

} // namespace

void ElectricSystem::step() {
    const auto t0 = std::chrono::steady_clock::now();
    ++tick_;
    const long long splitsTotal = stats_.splitsTotal;
    stats_ = ElectricStats{};
    stats_.splitsTotal = splitsTotal;
    if (!params_.enabled) {
        fields_.clear();
        strikes_.clear();
        maxV_ = 0.0f;
        return;
    }
    buildGrid();
    if (gw_ == 0) return;
    flood();
    const auto tFlood = std::chrono::steady_clock::now();
    const int ncomp = static_cast<int>(compStart_.size()) - 1;
    compIters_.assign(static_cast<std::size_t>(std::max(ncomp, 0)), 0);
    compResidual_.assign(static_cast<std::size_t>(std::max(ncomp, 0)), 0.0);
    compImbalance_.assign(static_cast<std::size_t>(std::max(ncomp, 0)), 0.0);
    compSettled_.assign(static_cast<std::size_t>(std::max(ncomp, 0)), 1);
    // small components side by side (each serial), big ones one by one with parallel blocks
    std::vector<int> small, big;
    for (int k = 0; k < ncomp; ++k)
        (static_cast<std::size_t>(compStart_[k + 1] - compStart_[k]) >= kBigComponent ? big : small).push_back(k);
    if (pool_ && small.size() > 1) pool_->parallelFor(static_cast<int>(small.size()), [&](int s) { solve(small[static_cast<std::size_t>(s)], false); });
    else for (int k : small) solve(k, false);
    for (int k : big) solve(k, true);
    for (int k = 0; k < ncomp; ++k) {
        stats_.solved += compIters_[k] > 0 || compResidual_[k] > 0.0 ? 1 : 0;
        stats_.iterations += compIters_[k];
        stats_.residual = std::max(stats_.residual, compResidual_[k]);
        stats_.imbalance = std::max(stats_.imbalance, compImbalance_[k]);
        stats_.unsettled += compSettled_[k] ? 0 : 1;
    }
    nodeComp_.resize(npx_.size());
    for (int k = 0; k < ncomp; ++k)
        std::fill(nodeComp_.begin() + compStart_[k], nodeComp_.begin() + compStart_[k + 1], k);
    const auto tSolve = std::chrono::steady_clock::now();
    effects();
    const auto tEffects = std::chrono::steady_clock::now();
    stats_.floodMs = std::chrono::duration<double, std::milli>(tFlood - t0).count();
    stats_.solveMs = std::chrono::duration<double, std::milli>(tSolve - tFlood).count();
    stats_.effectsMs = std::chrono::duration<double, std::milli>(tEffects - tSolve).count();
    std::vector<Strike> strikes;
    strikes.swap(strikes_);
    for (const Strike& s : strikes) runStrike(s);
    for (std::int32_t px : npx_) nodeOf_[static_cast<std::size_t>(px)] = -1;
    stats_.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void ElectricSystem::flood() {
    const int nchunks = cw_ * ch_;
    seeds_.resize(static_cast<std::size_t>(nchunks));
    lit_.resize(static_cast<std::size_t>(nchunks));
    // 1. fixed-potential pixels and lit lamps, per chunk (parallel, merged in chunk order)
    auto scan = [&](int k) {
        auto& s = seeds_[static_cast<std::size_t>(k)];
        auto& l = lit_[static_cast<std::size_t>(k)];
        s.clear();
        l.clear();
        const Chunk* c = grid_[static_cast<std::size_t>(k)];
        const int ox = (k % cw_) * kChunkSize, oy = (k / cw_) * kChunkSize;
        if (!c) {
            for (int y = 0; y < kChunkSize; ++y)
                std::memset(&cond_[static_cast<std::size_t>(oy + y) * gw_ + ox], 0, kChunkSize);
            return;
        }
        for (int i = 0; i < kChunkCells; ++i) {
            const Cell& cell = c->cells[i];
            const ElectricProps& ep = props_[cell.material];
            const int gx = ox + (i & kChunkMask), gy = oy + (i >> kChunkShift);
            const bool body = (cell.flags & kFlagBody) != 0;
            cond_[static_cast<std::size_t>(gy) * gw_ + gx] =
                !body && ep.conduct > 0.0f && (ep.role != ElectricRole::Plate || loaded(gx, gy)) ? 1 : 0;
            const ElectricRole r = ep.role;
            if (r == ElectricRole::None || body) continue;
            if (r == ElectricRole::Source || r == ElectricRole::Drain || r == ElectricRole::Thermo) s.push_back(static_cast<std::uint16_t>(i));
            else if (r == ElectricRole::LampLit) l.push_back(static_cast<std::uint16_t>(i));
        }
    };
    if (pool_) pool_->parallelFor(nchunks, scan);
    else for (int k = 0; k < nchunks; ++k) scan(k);

    // 2. flood-fill components from the seeds, in order
    npx_.clear();
    nx_.clear();
    ny_.clear();
    compStart_.assign(1, 0);
    auto add = [&](int x, int y) {
        const std::size_t px = static_cast<std::size_t>(y) * static_cast<std::size_t>(gw_) + static_cast<std::size_t>(x);
        nodeOf_[px] = static_cast<std::int32_t>(npx_.size());
        npx_.push_back(static_cast<std::int32_t>(px));
        nx_.push_back(static_cast<std::int16_t>(x));
        ny_.push_back(static_cast<std::int16_t>(y));
    };
    for (int k = 0; k < nchunks; ++k) {
        const int ocx = (k % cw_) * kChunkSize, ocy = (k / cw_) * kChunkSize;
        for (std::uint16_t li : seeds_[static_cast<std::size_t>(k)]) {
            const int sx = ocx + (li & kChunkMask), sy = ocy + (li >> kChunkShift);
            if (nodeOf_[static_cast<std::size_t>(sy) * gw_ + sx] >= 0) continue;
            const std::size_t first = npx_.size();
            add(sx, sy);
            for (std::size_t head = first; head < npx_.size(); ++head) {
                const int x = nx_[head], y = ny_[head];
                for (int d = 0; d < 4; ++d) {
                    const int qx = x + kDx[d], qy = y + kDy[d];
                    if (qx < 0 || qy < 0 || qx >= gw_ || qy >= gh_) continue;
                    const std::size_t qpx = static_cast<std::size_t>(qy) * gw_ + qx;
                    if (!cond_[qpx] || nodeOf_[qpx] >= 0) continue;
                    add(qx, qy);
                }
            }
            compStart_.push_back(static_cast<std::int32_t>(npx_.size()));
        }
    }
    const std::size_t n = npx_.size();
    stats_.nodes = static_cast<int>(n);
    stats_.components = static_cast<int>(compStart_.size()) - 1;

    // 3. node data: conductance, fixed potentials, warm start (pure per node: parallel blocks)
    ng_.resize(n); nfixed_.resize(n); nv_.resize(n);
    nr_.resize(n); nz_.resize(n); np_.resize(n); nq_.resize(n); nd_.resize(n);
    nnb_.resize(n * 4);
    nge_.resize(n * 4);
    nfresh_.assign(n, 0);
    nmat_.resize(n);
    const int bcx = gx0_ >> kChunkShift, bcy = gy0_ >> kChunkShift;
    overBlocks(pool_, n, [&](std::size_t i0, std::size_t i1) {
        const Field* lastField = nullptr;
        std::uint64_t lastKey = ~0ull;
        for (std::size_t i = i0; i < i1; ++i) {
            const int x = nx_[i], y = ny_[i];
            const Cell& c = *gridCell(x, y);
            const ElectricProps& p = props_[c.material];
            nmat_[i] = c.material;
            ng_[i] = p.conduct;
            nfixed_[i] = p.fixed() ? 1 : 0;
            if (p.role == ElectricRole::Source) nv_[i] = p.voltage;
            else if (p.role == ElectricRole::Drain) nv_[i] = 0.0;
            else if (p.role == ElectricRole::Thermo)
                nv_[i] = std::clamp(p.voltage * (c.temperature - kAmbientTemp), -params_.thermoMax, params_.thermoMax);
            else {
                const std::uint64_t key = chunkKey(bcx + (x >> kChunkShift), bcy + (y >> kChunkShift));
                if (key != lastKey) {
                    auto it = fields_.find(key);
                    lastField = it == fields_.end() ? nullptr : &it->second;
                    lastKey = key;
                }
                const float v = lastField ? lastField->v[static_cast<std::size_t>(((y & kChunkMask) << kChunkShift) | (x & kChunkMask))]
                                          : std::numeric_limits<float>::quiet_NaN();
                if (std::isnan(v)) { nv_[i] = 0.0; nfresh_[i] = 1; }
                else nv_[i] = v;
            }
        }
        return SumMax{};
    });
    // 4. edges (needs every node's conductance)
    overBlocks(pool_, n, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            const int x = nx_[i], y = ny_[i];
            for (int d = 0; d < 4; ++d) {
                const int qx = x + kDx[d], qy = y + kDy[d];
                std::int32_t j = -1;
                if (qx >= 0 && qy >= 0 && qx < gw_ && qy < gh_) j = nodeOf_[static_cast<std::size_t>(qy) * gw_ + qx];
                nnb_[i * 4 + d] = j;
                if (j < 0) { nge_[i * 4 + d] = 0.0f; continue; }
                const float a = ng_[i], b = ng_[static_cast<std::size_t>(j)];
                nge_[i * 4 + d] = 2.0f * a * b / (a + b);
            }
        }
        return SumMax{};
    });
    // 5. a pixel that just joined (a wire extended, a bubble's hole refilled) starts at its
    //    neighbours' potential, so a small change doesn't look like a big error to the solver
    for (std::size_t i = 0; i < n; ++i) {
        if (!nfresh_[i]) continue;
        double s = 0.0;
        int c = 0;
        for (int d = 0; d < 4; ++d) {
            const std::int32_t j = nnb_[i * 4 + d];
            if (j >= 0 && nfresh_[static_cast<std::size_t>(j)] != 1) { s += nv_[static_cast<std::size_t>(j)]; ++c; }
        }
        if (c > 0) nv_[i] = s / c;
        nfresh_[i] = 2; // filled (its value may seed the next fresh neighbour)
    }
}

// Two-level correction for big pieces: plain CG removes local errors fast but a smooth,
// piece-wide error (a switch closing between a 100 V half and a 0 V half) takes thousands of
// iterations. Free pixels are grouped into 8x8 px aggregates; the Galerkin coarse system
// (P^T A P, P piecewise constant) is solved for the residual and the correction is added back.
// It minimises the energy error over the coarse space, so it never makes the field worse.
// Serial and in node order: deterministic.
void ElectricSystem::coarseCorrect(int comp) {
    const std::size_t b = static_cast<std::size_t>(compStart_[comp]), e = static_cast<std::size_t>(compStart_[comp + 1]);
    const std::size_t m = e - b;
    const int aw = (gw_ + 7) >> 3, ah = (gh_ + 7) >> 3;
    std::vector<std::int32_t> aggOf(static_cast<std::size_t>(aw) * ah, -1);
    std::vector<std::int32_t> agg(m, -1);
    std::vector<std::int32_t> aggBlock;
    for (std::size_t k = 0; k < m; ++k) {
        const std::size_t i = b + k;
        if (nfixed_[i]) continue;
        const std::size_t blk = static_cast<std::size_t>(ny_[i] >> 3) * aw + static_cast<std::size_t>(nx_[i] >> 3);
        if (aggOf[blk] < 0) { aggOf[blk] = static_cast<std::int32_t>(aggBlock.size()); aggBlock.push_back(static_cast<std::int32_t>(blk)); }
        agg[k] = aggOf[blk];
    }
    const std::size_t na = aggBlock.size();
    if (na < 2) return;
    std::vector<double> cd(na, 0.0), rc(na, 0.0), ce(na, 0.0), cr(na), cz(na), cp(na), cq(na);
    std::vector<std::int32_t> cnb(na * 4, -1);
    std::vector<double> cge(na * 4, 0.0);
    const double* v = nv_.data();
    for (std::size_t k = 0; k < m; ++k) {
        const std::int32_t a = agg[k];
        if (a < 0) continue;
        const std::size_t i = b + k;
        double res = 0.0;
        for (int t = 0; t < 4; ++t) {
            const std::int32_t j = nnb_[i * 4 + t];
            if (j < 0) continue;
            const double g = nge_[i * 4 + t];
            cd[static_cast<std::size_t>(a)] += g;
            res += g * (v[j] - v[i]);
            const std::int32_t aj = agg[static_cast<std::size_t>(j) - b];
            if (aj < 0) continue;                                // a terminal: Dirichlet, diagonal only
            if (aj == a) { cd[static_cast<std::size_t>(a)] -= g; continue; } // inside the aggregate
            const int bi = aggBlock[static_cast<std::size_t>(a)], bj = aggBlock[static_cast<std::size_t>(aj)];
            const int dx = bj % aw - bi % aw, dy = bj / aw - bi / aw;
            const int dir = dx > 0 ? 0 : dx < 0 ? 1 : dy > 0 ? 2 : 3;
            cnb[static_cast<std::size_t>(a) * 4 + dir] = aj;
            cge[static_cast<std::size_t>(a) * 4 + dir] += g;
        }
        rc[static_cast<std::size_t>(a)] += res;
    }
    // coarse PCG for A_c ce = rc (A_c = diag cd, off-diagonal -cge)
    double rz = 0.0, rr0 = 0.0;
    for (std::size_t a = 0; a < na; ++a) {
        if (cd[a] <= 0.0) cd[a] = 1.0;
        cr[a] = rc[a];
        cz[a] = cr[a] / cd[a];
        cp[a] = cz[a];
        rz += cr[a] * cz[a];
        rr0 += cr[a] * cr[a];
    }
    if (rr0 <= 0.0) return;
    for (int it = 0; it < params_.coarseIterations; ++it) {
        double pq = 0.0;
        for (std::size_t a = 0; a < na; ++a) {
            double s = cd[a] * cp[a];
            for (int t = 0; t < 4; ++t) {
                const std::int32_t c = cnb[a * 4 + t];
                if (c >= 0) s -= cge[a * 4 + t] * cp[static_cast<std::size_t>(c)];
            }
            cq[a] = s;
            pq += cp[a] * s;
        }
        if (pq <= 0.0) break;
        const double alpha = rz / pq;
        double rzNew = 0.0, rr = 0.0;
        for (std::size_t a = 0; a < na; ++a) {
            ce[a] += alpha * cp[a];
            cr[a] -= alpha * cq[a];
            cz[a] = cr[a] / cd[a];
            rzNew += cr[a] * cz[a];
            rr += cr[a] * cr[a];
        }
        if (rr <= 1e-12 * rr0) break;
        const double beta = rzNew / rz;
        rz = rzNew;
        for (std::size_t a = 0; a < na; ++a) cp[a] = cz[a] + beta * cp[a];
    }
    for (std::size_t k = 0; k < m; ++k)
        if (agg[k] >= 0) nv_[b + k] += ce[static_cast<std::size_t>(agg[k])];
}

void ElectricSystem::solve(int comp, bool parallel) {
    ThreadPool* pool = parallel ? pool_ : nullptr;
    const std::size_t b = static_cast<std::size_t>(compStart_[comp]), e = static_cast<std::size_t>(compStart_[comp + 1]);
    double vmin = 1e300, vmax = -1e300;
    for (std::size_t i = b; i < e; ++i)
        if (nfixed_[i]) { vmin = std::min(vmin, nv_[i]); vmax = std::max(vmax, nv_[i]); }
    if (vmax - vmin < 1e-9) {
        // one potential: the whole component sits at it and carries no current
        for (std::size_t i = b; i < e; ++i) nv_[i] = vmin;
        return;
    }
    const double tol = params_.tolerance * (vmax - vmin);
    const double settledTol = params_.settled * (vmax - vmin);
    // Kirchhoff on the free nodes: the net current out of each is 0. Jacobi-preconditioned CG,
    // r = -A v, on node indices b..e (the scratch vectors are shared; components use disjoint ranges)
    double* v = nv_.data();
    const std::size_t m = e - b;
    double* r = nr_.data() + b; double* z = nz_.data() + b; double* p = np_.data() + b;
    double* q = nq_.data() + b; double* dg = nd_.data() + b;
    auto initPass = [&]() { return overBlocks(pool, m, [&](std::size_t k0, std::size_t k1) {
        SumMax s;
        for (std::size_t k = k0; k < k1; ++k) {
            const std::size_t i = b + k;
            double d = 0.0, res = 0.0;
            for (int t = 0; t < 4; ++t) {
                const std::int32_t j = nnb_[i * 4 + t];
                if (j < 0) continue;
                d += nge_[i * 4 + t];
                res += nge_[i * 4 + t] * (v[j] - v[i]);
            }
            dg[k] = d > 0.0 ? d : 1.0;
            r[k] = nfixed_[i] ? 0.0 : res;
            z[k] = r[k] / dg[k];
            p[k] = z[k];
            s.sum += r[k] * z[k];
            s.max = std::max(s.max, std::abs(z[k]));
            s.sum2 += r[k];                           // net current into the free nodes (0 when solved)
            if (nfixed_[i]) s.sum3 += 0.5 * std::abs(res); // current through the terminals
        }
        return s;
    }); };
    SumMax s0 = initPass();
    double rz = s0.sum, worst = s0.max;
    // Local corrections alone can look tiny while the field is globally off (a fresh wire that
    // starts at the source voltage everywhere): Kirchhoff over the whole piece must hold too.
    double current = std::max(s0.sum3, 1e-9);
    double imbalance = std::abs(s0.sum2);
    auto close = [&](double local, double global) { return worst <= local && imbalance <= global * current; };
    if (static_cast<int>(m) >= params_.coarseMin && !close(tol, 1e-3) && !close(settledTol, params_.settled)) {
        coarseCorrect(comp);
        s0 = initPass();
        rz = s0.sum;
        worst = s0.max;
        current = std::max(s0.sum3, 1e-9);
        imbalance = std::abs(s0.sum2);
    }
    int it = 0;
    // already close enough (warm start, nothing much changed): keep it, don't polish
    for (; it < params_.maxIterations && !close(tol, 1e-3) && !(it == 0 && close(settledTol, params_.settled)); ++it) {
        const double pq = overBlocks(pool, m, [&](std::size_t k0, std::size_t k1) {
            SumMax s;
            for (std::size_t k = k0; k < k1; ++k) {
                const std::size_t i = b + k;
                if (nfixed_[i]) { q[k] = 0.0; continue; }
                double a = dg[k] * p[k];
                for (int t = 0; t < 4; ++t) {
                    const std::int32_t j = nnb_[i * 4 + t];
                    if (j >= 0) a -= nge_[i * 4 + t] * p[static_cast<std::size_t>(j) - b];
                }
                q[k] = a;
                s.sum += p[k] * a;
            }
            return s;
        }).sum;
        if (pq <= 0.0) break;
        const double alpha = rz / pq;
        const SumMax s1 = overBlocks(pool, m, [&](std::size_t k0, std::size_t k1) {
            SumMax s;
            for (std::size_t k = k0; k < k1; ++k) {
                v[b + k] += alpha * p[k];
                r[k] -= alpha * q[k];
                z[k] = r[k] / dg[k];
                s.sum += r[k] * z[k];
                s.max = std::max(s.max, std::abs(z[k]));
                s.sum2 += r[k];
            }
            return s;
        });
        const double beta = s1.sum / rz;
        rz = s1.sum;
        worst = s1.max;
        imbalance = std::abs(s1.sum2);
        overBlocks(pool, m, [&](std::size_t k0, std::size_t k1) {
            for (std::size_t k = k0; k < k1; ++k) p[k] = z[k] + beta * p[k];
            return SumMax{};
        });
    }
    compIters_[static_cast<std::size_t>(comp)] = it;
    compResidual_[static_cast<std::size_t>(comp)] = worst;
    compImbalance_[static_cast<std::size_t>(comp)] = imbalance / current;
    compSettled_[static_cast<std::size_t>(comp)] = close(settledTol, 2.0 * params_.settled) ? 1 : 0;
}

void ElectricSystem::effects() {
    const std::size_t n = npx_.size();
    const int nchunks = cw_ * ch_;
    const int bcx = gx0_ >> kChunkShift, bcy = gy0_ >> kChunkShift;
    auto chunkIndex = [&](std::size_t i) { return (ny_[i] >> kChunkShift) * cw_ + (nx_[i] >> kChunkShift); };

    // fields: the chunks with nodes keep (fresh) potentials, the others are dropped
    chunkHas_.assign(static_cast<std::size_t>(nchunks), 0);
    for (std::size_t i = 0; i < n; ++i) chunkHas_[static_cast<std::size_t>(chunkIndex(i))] = 1;
    fieldGrid_.assign(static_cast<std::size_t>(nchunks), nullptr);
    {
        std::vector<std::uint64_t> keep;
        std::vector<Field*> fill;
        for (int k = 0; k < nchunks; ++k) {
            if (!chunkHas_[static_cast<std::size_t>(k)]) continue;
            const std::uint64_t key = chunkKey(bcx + k % cw_, bcy + k / cw_);
            keep.push_back(key);
            Field& f = fields_[key];
            fill.push_back(&f);
            fieldGrid_[static_cast<std::size_t>(k)] = &f;
        }
        auto clearField = [&](int k) {
            Field& f = *fill[static_cast<std::size_t>(k)];
            std::fill(f.v.begin(), f.v.end(), std::numeric_limits<float>::quiet_NaN());
            std::fill(f.i.begin(), f.i.end(), 0.0f);
        };
        if (pool_ && fill.size() > 1) pool_->parallelFor(static_cast<int>(fill.size()), clearField);
        else for (std::size_t k = 0; k < fill.size(); ++k) clearField(static_cast<int>(k));
        std::sort(keep.begin(), keep.end());
        for (auto it = fields_.begin(); it != fields_.end();)
            it = std::binary_search(keep.begin(), keep.end(), it->first) ? std::next(it) : fields_.erase(it);
    }

    // power and current per node, the fields, and resistive heating: pure per node (parallel blocks)
    npow_.resize(n);
    ncur_.resize(n);
    std::vector<double> heatPart((n + kBlock - 1) / kBlock, 0.0);
    const SumMax totals = overBlocks(pool_, n, [&](std::size_t i0, std::size_t i1) {
        SumMax s;
        double heat = 0.0;
        for (std::size_t i = i0; i < i1; ++i) {
            double pw = 0.0, cur = 0.0;
            for (int d = 0; d < 4; ++d) {
                const std::int32_t j = nnb_[i * 4 + d];
                if (j < 0) continue;
                const double dv = nv_[i] - nv_[static_cast<std::size_t>(j)];
                const double f = nge_[i * 4 + d] * dv;
                pw += 0.5 * f * dv;
                cur += 0.5 * std::abs(f);
            }
            const bool settled = compSettled_[static_cast<std::size_t>(nodeComp_[i])] != 0;
            npow_[i] = static_cast<float>(pw);
            ncur_[i] = static_cast<float>(cur);
            s.sum += pw;
            s.max = std::max(s.max, std::abs(nv_[i]));
            const int x = nx_[i], y = ny_[i];
            Field& f = *fieldGrid_[static_cast<std::size_t>(chunkIndex(i))];
            const std::size_t l = static_cast<std::size_t>(((y & kChunkMask) << kChunkShift) | (x & kChunkMask));
            f.v[l] = static_cast<float>(nv_[i]);
            f.i[l] = settled ? ncur_[i] : 0.0f;
            // resistive heating (the terminals themselves stay cool; a half-solved field waits)
            if (!settled || nfixed_[i] || pw <= 0.0) continue;
            Cell& c = *gridCell(x, y);
            const Material& m = mats_[c.material];
            const float dT = std::min(params_.heatGain * props_[c.material].heatShare * static_cast<float>(pw) / m.heatCapacity, params_.maxHeat);
            if (dT <= 1e-5f) continue;
            const float before = c.temperature;
            c.temperature = std::clamp(c.temperature + dT, kMinTemp, kMaxTemp);
            heat += static_cast<double>(c.temperature - before) * m.heatCapacity;
        }
        heatPart[i0 / kBlock] = heat;
        return s;
    });
    stats_.power = totals.sum;
    maxV_ = static_cast<float>(totals.max);
    for (double h : heatPart) stats_.heat += h;
    // heat wakes: every chunk with live pixels, and its left and upper neighbours (they own the pairs across the seam)
    for (int k = 0; k < nchunks; ++k) {
        if (!chunkHas_[static_cast<std::size_t>(k)]) continue;
        const int kx = k % cw_, ky = k / cw_;
        for (int dy = -1; dy <= 0; ++dy)
            for (int dx = -1; dx <= 0; ++dx) {
                if (kx + dx < 0 || ky + dy < 0) continue;
                if (Chunk* c = grid_[static_cast<std::size_t>(ky + dy) * cw_ + (kx + dx)]) c->heatNext.store(true, std::memory_order_relaxed);
            }
    }

    // lamps, sparks and electrolysis: serial, in node order, one RNG stream
    Rng rng(Rng::hash(seed_ ^ 0xE1EC7u, tick_));
    const std::uint32_t lampStamp = ++markStamp_;
    std::vector<std::size_t> clump;
    auto isLamp = [&](MaterialId m) {
        const ElectricRole r = props_[m].role;
        return (r == ElectricRole::Lamp || r == ElectricRole::LampLit) && props_[m].toggle != kEmpty;
    };
    for (std::size_t i = 0; i < n; ++i) {
        if (!serialRole_[nmat_[i]] && npow_[i] < params_.sparkPower) continue; // nothing to do for most wire
        if (!compSettled_[static_cast<std::size_t>(nodeComp_[i])]) {
            const Cell& c = *gridCell(nx_[i], ny_[i]);
            if (props_[c.material].role == ElectricRole::LampLit) ++stats_.lit; // stays as it is until the solve settles
            continue;
        }
        const int x = nx_[i], y = ny_[i];
        Cell& c = *gridCell(x, y);
        const ElectricProps& p = props_[c.material];
        const float pw = npow_[i];
        // a lamp lights as a whole: the mean power over its connected lamp pixels decides
        if (isLamp(c.material) && mark_[static_cast<std::size_t>(npx_[i])] != lampStamp) {
            clump.assign(1, i);
            mark_[static_cast<std::size_t>(npx_[i])] = lampStamp;
            double sum = 0.0;
            int litNow = 0;
            for (std::size_t h = 0; h < clump.size(); ++h) {
                const std::size_t a = clump[h];
                sum += npow_[a];
                litNow += props_[gridCell(nx_[a], ny_[a])->material].role == ElectricRole::LampLit ? 1 : 0;
                for (int d = 0; d < 4; ++d) {
                    const std::int32_t j = nnb_[a * 4 + d];
                    if (j < 0) continue;
                    const std::size_t pj = static_cast<std::size_t>(npx_[static_cast<std::size_t>(j)]);
                    if (mark_[pj] == lampStamp || !isLamp(gridCell(nx_[static_cast<std::size_t>(j)], ny_[static_cast<std::size_t>(j)])->material)) continue;
                    mark_[pj] = lampStamp;
                    clump.push_back(static_cast<std::size_t>(j));
                }
            }
            const double mean = sum / static_cast<double>(clump.size());
            const bool wasLit = 2 * litNow >= static_cast<int>(clump.size());
            const bool lit = mean >= (wasLit ? 0.5 : 1.0) * params_.lampPower;
            for (std::size_t a : clump) {
                const int ax = nx_[a], ay = ny_[a];
                Cell& lc = *gridCell(ax, ay);
                const bool isLit = props_[lc.material].role == ElectricRole::LampLit;
                if (isLit != lit) {
                    lc.material = props_[lc.material].toggle;
                    wakeAt(ax, ay, true);
                    ++stats_.toggled;
                }
                if (lit) ++stats_.lit;
            }
        }
        // sparks into the air next to a pixel carrying a lot of power
        if (spark_ != kEmpty && pw >= params_.sparkPower) {
            const float chance = params_.sparkChance * std::min(pw / params_.sparkPower, 5.0f);
            if (rng.unit() < chance) {
                const int d = static_cast<int>(rng.below(4));
                Cell* t = gridCell(x + kDx[d], y + kDy[d]);
                if (t && t->material == kEmpty && !(t->flags & kFlagBody)) {
                    const float temp = std::max(t->temperature, mats_[spark_].spawnTemp);
                    *t = mats_[spark_].spawn(static_cast<std::uint8_t>(rng.below(256)));
                    t->temperature = temp;
                    wakeAt(x + kDx[d], y + kDy[d], true);
                    ++stats_.sparks;
                }
            }
        }
        // electrolysis: this cell and a touching one of the same liquid split into the two products
        if (p.splitA != kEmpty && ncur_[i] >= p.splitCurrent) {
            const float chance = params_.splitRate * ncur_[i] / p.splitCurrent;
            if (rng.unit() < chance) {
                const int d0 = static_cast<int>(rng.below(4));
                for (int k = 0; k < 4; ++k) {
                    const int d = (d0 + k) & 3;
                    Cell* t = gridCell(x + kDx[d], y + kDy[d]);
                    if (!t || t->material != c.material || (t->flags & kFlagBody)) continue;
                    split(c, *t, p);
                    wakeAt(x, y, true);
                    wakeAt(x + kDx[d], y + kDy[d], true);
                    ++stats_.splits;
                    ++stats_.splitsTotal;
                    break;
                }
            }
        }
    }
    // lit lamps that aren't powered any more (not part of any component) go out
    for (int k = 0; k < nchunks; ++k) {
        Chunk* ck = grid_[static_cast<std::size_t>(k)];
        if (!ck) continue;
        const int ocx = (k % cw_) * kChunkSize, ocy = (k / cw_) * kChunkSize;
        for (std::uint16_t li : lit_[static_cast<std::size_t>(k)]) {
            const int x = ocx + (li & kChunkMask), y = ocy + (li >> kChunkShift);
            if (nodeOf_[static_cast<std::size_t>(y) * gw_ + x] >= 0) continue; // handled above
            Cell& c = ck->cells[li];
            const ElectricProps& p = props_[c.material];
            if (p.role != ElectricRole::LampLit || p.toggle == kEmpty) continue;
            c.material = p.toggle;
            wakeAt(x, y, true);
            ++stats_.toggled;
        }
    }
}

void ElectricSystem::split(Cell& a, Cell& b, const ElectricProps& p) {
    auto become = [](Cell& cell, MaterialId into) {
        cell.material = into;
        cell.flags = 0;
        cell.life = 0;
        cell.aux = 0;
        cell.vx = cell.vy = 0;
    };
    become(a, p.splitA);
    become(b, p.splitB);
}

void ElectricSystem::runStrike(const Strike& s) {
    Rng rng(Rng::hash(seed_ ^ 0x5791CEu, tick_ * 131u + static_cast<std::uint64_t>(stats_.strikes)));
    ++stats_.strikes;
    int gx = s.x - gx0_, gy = s.y - gy0_;
    int hx = -1, hy = -1;
    for (int k = 0; k < params_.strikeLength; ++k) {
        Cell* c = gridCell(gx, gy);
        if (!c) break;
        const Material& m = mats_[c->material];
        if (!(m.kind == MaterialKind::Empty || m.kind == MaterialKind::Gas || m.kind == MaterialKind::Special) ||
            (c->flags & kFlagBody)) { hx = gx; hy = gy; break; }
        if (spark_ != kEmpty && c->material == kEmpty && rng.below(4) != 0) {
            *c = mats_[spark_].spawn(static_cast<std::uint8_t>(rng.below(256)));
            wakeAt(gx, gy, true);
            ++stats_.sparks;
        } else if (m.kind == MaterialKind::Gas) {
            heatCell(gx, gy, *c, params_.strikeHeat * 0.2f); // lights flammable gas on the way down
        }
        gy += 1;
        const std::uint32_t r = rng.below(8);
        if (r == 0) gx -= 1;
        else if (r == 1) gx += 1;
    }
    if (hx < 0) return;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if (Cell* c = gridCell(hx + dx, hy + dy))
                heatCell(hx + dx, hy + dy, *c, params_.strikeHeat * (dx == 0 && dy == 0 ? 1.0f : 0.3f));
    // the discharge spreads through the conductors it hit, nearest first
    const Cell* hit = gridCell(hx, hy);
    if (!hit || props_[hit->material].conduct <= 0.0f || (hit->flags & kFlagBody)) return;
    const std::uint32_t stamp = ++markStamp_;
    std::vector<std::int32_t> q;
    q.push_back(hy * gw_ + hx);
    mark_[static_cast<std::size_t>(q[0])] = stamp;
    for (std::size_t head = 0; head < q.size() && static_cast<int>(q.size()) < params_.strikeReach; ++head) {
        const int x = q[head] % gw_, y = q[head] / gw_;
        for (int d = 0; d < 4 && static_cast<int>(q.size()) < params_.strikeReach; ++d) {
            const int nx = x + kDx[d], ny = y + kDy[d];
            if (nx < 0 || ny < 0 || nx >= gw_ || ny >= gh_) continue;
            const std::size_t npx = static_cast<std::size_t>(ny) * gw_ + nx;
            if (mark_[npx] == stamp) continue;
            const Cell* c = gridCell(nx, ny);
            if (!c || (c->flags & kFlagBody) || props_[c->material].conduct <= 0.0f) continue;
            mark_[npx] = stamp;
            q.push_back(static_cast<std::int32_t>(npx));
        }
    }
    const float reach = static_cast<float>(q.size());
    for (std::size_t k = 0; k < q.size(); ++k) {
        const int x = q[k] % gw_, y = q[k] / gw_;
        Cell& c = *gridCell(x, y);
        const float f = 1.0f - static_cast<float>(k) / reach;
        heatCell(x, y, c, params_.strikeNodeHeat * f);
        Field& fl = fieldAt(x, y);
        const std::size_t l = static_cast<std::size_t>(((y & kChunkMask) << kChunkShift) | (x & kChunkMask));
        fl.i[l] += params_.strikeCurrent * f;
        const ElectricProps& p = props_[c.material];
        if (p.splitA != kEmpty && rng.unit() < params_.strikeSplit * f) {
            for (int d = 0; d < 4; ++d) {
                Cell* t = gridCell(x + kDx[d], y + kDy[d]);
                if (!t || t->material != c.material || (t->flags & kFlagBody)) continue;
                c.material = p.splitA; c.flags = 0; c.life = 0; c.aux = 0; c.vx = c.vy = 0;
                t->material = p.splitB; t->flags = 0; t->life = 0; t->aux = 0; t->vx = t->vy = 0;
                wakeAt(x, y, true);
                wakeAt(x + kDx[d], y + kDy[d], true);
                ++stats_.splits;
                ++stats_.splitsTotal;
                break;
            }
        }
    }
}

// --------------------------------------------------------------------------
// player-facing

int ElectricSystem::toggleSwitch(int x, int y, int radius) {
    buildGrid();
    if (gw_ == 0) return 0;
    auto isSwitch = [&](int gx, int gy) {
        const Cell* c = gridCell(gx, gy);
        return c && !(c->flags & kFlagBody) && props_[c->material].role == ElectricRole::Switch && props_[c->material].toggle != kEmpty;
    };
    // the nearest switch pixel
    int bx = -1, by = -1, best = radius * radius + 1;
    const int cx = x - gx0_, cy = y - gy0_;
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
            const int d2 = dx * dx + dy * dy;
            if (d2 < best && isSwitch(cx + dx, cy + dy)) { best = d2; bx = cx + dx; by = cy + dy; }
        }
    if (bx < 0) return 0;
    const std::uint32_t stamp = ++markStamp_;
    std::vector<std::int32_t> q{by * gw_ + bx};
    mark_[static_cast<std::size_t>(q[0])] = stamp;
    for (std::size_t head = 0; head < q.size() && q.size() < 4096; ++head) {
        const int qx = q[head] % gw_, qy = q[head] / gw_;
        for (int d = 0; d < 4; ++d) {
            const int nx = qx + kDx[d], ny = qy + kDy[d];
            if (!isSwitch(nx, ny)) continue;
            const std::size_t npx = static_cast<std::size_t>(ny) * gw_ + nx;
            if (mark_[npx] == stamp) continue;
            mark_[npx] = stamp;
            q.push_back(static_cast<std::int32_t>(npx));
        }
    }
    for (std::int32_t px : q) {
        const int qx = px % gw_, qy = px / gw_;
        Cell& c = *gridCell(qx, qy);
        c.material = props_[c.material].toggle;
        wakeAt(qx, qy, true);
    }
    return static_cast<int>(q.size());
}

float ElectricSystem::potentialAt(int x, int y) const {
    auto it = fields_.find(chunkKey(x >> kChunkShift, y >> kChunkShift));
    if (it == fields_.end()) return 0.0f;
    const float v = it->second.v[static_cast<std::size_t>(((y & kChunkMask) << kChunkShift) | (x & kChunkMask))];
    return std::isnan(v) ? 0.0f : v;
}

const float* ElectricSystem::potentials(int cx, int cy) const {
    auto it = fields_.find(chunkKey(cx, cy));
    return it == fields_.end() ? nullptr : it->second.v.data();
}

const float* ElectricSystem::currents(int cx, int cy) const {
    auto it = fields_.find(chunkKey(cx, cy));
    return it == fields_.end() ? nullptr : it->second.i.data();
}

float ElectricSystem::currentAt(int x, int y) const {
    auto it = fields_.find(chunkKey(x >> kChunkShift, y >> kChunkShift));
    if (it == fields_.end()) return 0.0f;
    return it->second.i[static_cast<std::size_t>(((y & kChunkMask) << kChunkShift) | (x & kChunkMask))];
}

// --------------------------------------------------------------------------
// streaming and saves

bool ElectricSystem::exportChunk(int cx, int cy, std::vector<std::uint8_t>& out, bool clear) {
    out.clear();
    auto it = fields_.find(chunkKey(cx, cy));
    if (it == fields_.end()) return false;
    ByteWriter w;
    std::uint32_t count = 0;
    for (float v : it->second.v) count += std::isnan(v) ? 0u : 1u;
    w.u32(count);
    for (int l = 0; l < kChunkCells; ++l) {
        const float v = it->second.v[static_cast<std::size_t>(l)];
        if (std::isnan(v)) continue;
        w.u16(static_cast<std::uint16_t>(l));
        w.f32(v);
    }
    out = std::move(w.buf);
    if (clear) fields_.erase(it);
    return true;
}

void ElectricSystem::importChunk(int cx, int cy, const std::vector<std::uint8_t>& blob) {
    if (blob.empty()) return;
    ByteReader r(blob.data(), blob.size());
    const std::uint32_t count = r.u32();
    if (!r.ok() || count > static_cast<std::uint32_t>(kChunkCells)) return;
    Field f;
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::uint16_t l = r.u16();
        const float v = r.f32();
        if (!r.ok() || l >= kChunkCells) return;
        f.v[l] = v;
    }
    fields_[chunkKey(cx, cy)] = std::move(f);
}

void ElectricSystem::clear() {
    fields_.clear();
    strikes_.clear();
    maxV_ = 0.0f;
}

} // namespace notita
