#include "Electric.h"

#include "ChunkCodec.h"
#include "Player.h"
#include "Rng.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

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
    if (mark_.size() != n) { mark_.assign(n, 0); markStamp_ = 0; }
}

Cell* ElectricSystem::gridCell(int gx, int gy) const {
    if (gx < 0 || gy < 0 || gx >= gw_ || gy >= gh_) return nullptr;
    Chunk* c = grid_[static_cast<std::size_t>(gy >> kChunkShift) * cw_ + (gx >> kChunkShift)];
    return c ? &c->at(gx & kChunkMask, gy & kChunkMask) : nullptr;
}

bool ElectricSystem::loaded(int gx, int gy) const {
    if (const Cell* above = gridCell(gx, gy - 1)) {
        const MaterialKind k = mats_[above->material].kind;
        if (k == MaterialKind::Solid || k == MaterialKind::Powder || k == MaterialKind::Liquid) return true;
    }
    return player_ && player_->covers(gx + gx0_, gy + gy0_ - 1);
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
    const int ncomp = static_cast<int>(compStart_.size()) - 1;
    compIters_.assign(static_cast<std::size_t>(std::max(ncomp, 0)), 0);
    compResidual_.assign(static_cast<std::size_t>(std::max(ncomp, 0)), 0.0);
    compSettled_.assign(static_cast<std::size_t>(std::max(ncomp, 0)), 1);
    // components are independent: solve them in parallel (each one is serial inside)
    if (pool_ && ncomp > 1) pool_->parallelFor(ncomp, [&](int k) { solve(k); });
    else for (int k = 0; k < ncomp; ++k) solve(k);
    for (int k = 0; k < ncomp; ++k) {
        stats_.solved += compIters_[k] > 0 || compResidual_[k] > 0.0 ? 1 : 0;
        stats_.iterations += compIters_[k];
        stats_.residual = std::max(stats_.residual, compResidual_[k]);
        stats_.unsettled += compSettled_[k] ? 0 : 1;
    }
    nodeComp_.resize(npx_.size());
    for (int k = 0; k < ncomp; ++k)
        std::fill(nodeComp_.begin() + compStart_[k], nodeComp_.begin() + compStart_[k + 1], k);
    effects();
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
        if (!c) return;
        for (int i = 0; i < kChunkCells; ++i) {
            const Cell& cell = c->cells[i];
            const ElectricRole r = props_[cell.material].role;
            if (r == ElectricRole::None || (cell.flags & kFlagBody)) continue;
            if (r == ElectricRole::Source || r == ElectricRole::Drain || r == ElectricRole::Thermo) s.push_back(static_cast<std::uint16_t>(i));
            else if (r == ElectricRole::LampLit) l.push_back(static_cast<std::uint16_t>(i));
        }
    };
    if (pool_) pool_->parallelFor(nchunks, scan);
    else for (int k = 0; k < nchunks; ++k) scan(k);

    // 2. flood-fill components from the seeds, in order
    npx_.clear(); ng_.clear(); nfixed_.clear(); nv_.clear();
    compStart_.assign(1, 0);
    for (int k = 0; k < nchunks; ++k) {
        const int ocx = (k % cw_) * kChunkSize, ocy = (k / cw_) * kChunkSize;
        for (std::uint16_t li : seeds_[static_cast<std::size_t>(k)]) {
            const int sx = ocx + (li & kChunkMask), sy = ocy + (li >> kChunkShift);
            const std::size_t spx = static_cast<std::size_t>(sy) * gw_ + sx;
            if (nodeOf_[spx] >= 0) continue;
            const std::int32_t first = static_cast<std::int32_t>(npx_.size());
            nodeOf_[spx] = first;
            npx_.push_back(static_cast<std::int32_t>(spx));
            for (std::size_t head = static_cast<std::size_t>(first); head < npx_.size(); ++head) {
                const int px = npx_[head];
                const int x = px % gw_, y = px / gw_;
                for (int d = 0; d < 4; ++d) {
                    const int nx = x + kDx[d], ny = y + kDy[d];
                    if (nx < 0 || ny < 0 || nx >= gw_ || ny >= gh_) continue;
                    const std::size_t npx = static_cast<std::size_t>(ny) * gw_ + nx;
                    if (nodeOf_[npx] >= 0 || !conductsAt(nx, ny)) continue;
                    nodeOf_[npx] = static_cast<std::int32_t>(npx_.size());
                    npx_.push_back(static_cast<std::int32_t>(npx));
                }
            }
            compStart_.push_back(static_cast<std::int32_t>(npx_.size()));
        }
    }
    const std::size_t n = npx_.size();
    stats_.nodes = static_cast<int>(n);
    stats_.components = static_cast<int>(compStart_.size()) - 1;

    // 3. node data: conductance, fixed potentials, warm start, edges
    ng_.resize(n); nfixed_.resize(n); nv_.resize(n);
    nr_.resize(n); nz_.resize(n); np_.resize(n); nq_.resize(n); nd_.resize(n);
    nnb_.assign(n * 4, -1);
    nge_.assign(n * 4, 0.0f);
    const int bcx = gx0_ >> kChunkShift, bcy = gy0_ >> kChunkShift;
    const Field* lastField = nullptr;
    std::uint64_t lastKey = ~0ull;
    for (std::size_t i = 0; i < n; ++i) {
        const int px = npx_[i], x = px % gw_, y = px / gw_;
        const Cell& c = *gridCell(x, y);
        const ElectricProps& p = props_[c.material];
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
            nv_[i] = lastField ? lastField->v[static_cast<std::size_t>(((y & kChunkMask) << kChunkShift) | (x & kChunkMask))] : 0.0;
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        const int px = npx_[i], x = px % gw_, y = px / gw_;
        for (int d = 0; d < 4; ++d) {
            const int nx = x + kDx[d], ny = y + kDy[d];
            if (nx < 0 || ny < 0 || nx >= gw_ || ny >= gh_) continue;
            const std::int32_t j = nodeOf_[static_cast<std::size_t>(ny) * gw_ + nx];
            if (j < 0) continue;
            nnb_[i * 4 + d] = j;
            const float a = ng_[i], b = ng_[static_cast<std::size_t>(j)];
            nge_[i * 4 + d] = 2.0f * a * b / (a + b);
        }
    }
}

void ElectricSystem::solve(int comp) {
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
    // r = -A x on the free nodes (Kirchhoff: the net current out of every free node is 0)
    double* v = nv_.data();
    auto resid = [&](std::size_t i) {
        double r = 0.0;
        for (int d = 0; d < 4; ++d) {
            const std::int32_t j = nnb_[i * 4 + d];
            if (j >= 0) r += nge_[i * 4 + d] * (v[j] - v[i]);
        }
        return r;
    };
    const std::size_t m = e - b;
    // the scratch vectors are shared, but components use disjoint ranges of them
    double* r = nr_.data() + b; double* z = nz_.data() + b; double* p = np_.data() + b;
    double* q = nq_.data() + b; double* dg = nd_.data() + b;
    double rz = 0.0, worst = 0.0;
    for (std::size_t k = 0; k < m; ++k) {
        const std::size_t i = b + k;
        double d = 0.0;
        for (int s = 0; s < 4; ++s) d += nge_[i * 4 + s];
        dg[k] = d > 0.0 ? d : 1.0;
        r[k] = nfixed_[i] ? 0.0 : resid(i);
        z[k] = r[k] / dg[k];
        p[k] = z[k];
        rz += r[k] * z[k];
        worst = std::max(worst, std::abs(z[k]));
    }
    int it = 0;
    for (; it < params_.maxIterations && worst > tol; ++it) {
        double pq = 0.0;
        for (std::size_t k = 0; k < m; ++k) {
            const std::size_t i = b + k;
            if (nfixed_[i]) { q[k] = 0.0; continue; }
            double s = dg[k] * p[k];
            for (int d = 0; d < 4; ++d) {
                const std::int32_t j = nnb_[i * 4 + d];
                if (j >= 0) s -= nge_[i * 4 + d] * p[static_cast<std::size_t>(j) - b];
            }
            q[k] = s;
            pq += p[k] * s;
        }
        if (pq <= 0.0) break;
        const double alpha = rz / pq;
        double rzNew = 0.0;
        worst = 0.0;
        for (std::size_t k = 0; k < m; ++k) {
            v[b + k] += alpha * p[k];
            r[k] -= alpha * q[k];
            z[k] = r[k] / dg[k];
            rzNew += r[k] * z[k];
            worst = std::max(worst, std::abs(z[k]));
        }
        const double beta = rzNew / rz;
        rz = rzNew;
        for (std::size_t k = 0; k < m; ++k) p[k] = z[k] + beta * p[k];
    }
    compIters_[static_cast<std::size_t>(comp)] = it;
    compResidual_[static_cast<std::size_t>(comp)] = worst;
    compSettled_[static_cast<std::size_t>(comp)] = worst <= params_.settled * (vmax - vmin) ? 1 : 0;
}

void ElectricSystem::effects() {
    const std::size_t n = npx_.size();
    npow_.assign(n, 0.0f);
    ncur_.assign(n, 0.0f);
    maxV_ = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        double pw = 0.0, cur = 0.0;
        for (int d = 0; d < 4; ++d) {
            const std::int32_t j = nnb_[i * 4 + d];
            if (j < 0) continue;
            const double dv = nv_[i] - nv_[static_cast<std::size_t>(j)];
            const double f = nge_[i * 4 + d] * dv;
            pw += 0.5 * f * dv;
            cur += 0.5 * std::abs(f);
        }
        npow_[i] = static_cast<float>(pw);
        ncur_[i] = static_cast<float>(cur);
        maxV_ = std::max(maxV_, static_cast<float>(std::abs(nv_[i])));
        stats_.power += pw;
    }

    // fields: the chunks with nodes keep their potentials, the rest are dropped
    {
        std::map<std::uint64_t, Field> next;
        const int bcx = gx0_ >> kChunkShift, bcy = gy0_ >> kChunkShift;
        std::uint64_t lastKey = ~0ull;
        Field* f = nullptr;
        for (std::size_t i = 0; i < n; ++i) {
            const int px = npx_[i], x = px % gw_, y = px / gw_;
            const std::uint64_t key = chunkKey(bcx + (x >> kChunkShift), bcy + (y >> kChunkShift));
            if (key != lastKey) {
                auto it = next.find(key);
                if (it == next.end()) {
                    auto old = fields_.find(key);
                    if (old != fields_.end()) {
                        it = next.emplace(key, std::move(old->second)).first;
                        std::fill(it->second.v.begin(), it->second.v.end(), 0.0f);
                        std::fill(it->second.i.begin(), it->second.i.end(), 0.0f);
                    } else {
                        it = next.emplace(key, Field{}).first;
                    }
                }
                f = &it->second;
                lastKey = key;
            }
            const std::size_t l = static_cast<std::size_t>(((y & kChunkMask) << kChunkShift) | (x & kChunkMask));
            f->v[l] = static_cast<float>(nv_[i]);
            f->i[l] = compSettled_[static_cast<std::size_t>(nodeComp_[i])] ? ncur_[i] : 0.0f;
        }
        fields_.swap(next);
    }

    Rng rng(Rng::hash(seed_ ^ 0xE1EC7u, tick_));
    for (std::size_t i = 0; i < n; ++i) {
        const int px = npx_[i], x = px % gw_, y = px / gw_;
        Cell& c = *gridCell(x, y);
        const ElectricProps& p = props_[c.material];
        const float pw = npow_[i];
        if (!compSettled_[static_cast<std::size_t>(nodeComp_[i])]) {
            if (p.role == ElectricRole::LampLit) ++stats_.lit; // stays as it is until the solve settles
            continue;
        }
        // resistive heating (the terminals themselves stay cool)
        if (!nfixed_[i] && pw > 0.0f) heatCell(x, y, c, params_.heatGain * p.heatShare * pw);
        // lamps
        if (p.role == ElectricRole::Lamp && pw >= params_.lampPower && p.toggle != kEmpty) {
            c.material = p.toggle;
            wakeAt(x, y, true);
            ++stats_.toggled;
        } else if (p.role == ElectricRole::LampLit && pw < 0.5f * params_.lampPower && p.toggle != kEmpty) {
            c.material = p.toggle;
            wakeAt(x, y, true);
            ++stats_.toggled;
        }
        if (props_[c.material].role == ElectricRole::LampLit) ++stats_.lit;
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
                    const MaterialId a = p.splitA, bb = p.splitB;
                    auto become = [&](Cell& cell, MaterialId into) {
                        cell.material = into;
                        cell.flags = 0;
                        cell.life = 0;
                        cell.aux = 0;
                        cell.vx = cell.vy = 0;
                    };
                    become(c, a);
                    become(*t, bb);
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
    const int nchunks = cw_ * ch_;
    for (int k = 0; k < nchunks; ++k) {
        Chunk* ck = grid_[static_cast<std::size_t>(k)];
        if (!ck) continue;
        const int ocx = (k % cw_) * kChunkSize, ocy = (k / cw_) * kChunkSize;
        for (std::uint16_t li : lit_[static_cast<std::size_t>(k)]) {
            const int x = ocx + (li & kChunkMask), y = ocy + (li >> kChunkShift);
            const std::size_t px = static_cast<std::size_t>(y) * gw_ + x;
            if (nodeOf_[px] >= 0) continue; // handled above
            Cell& c = ck->cells[li];
            const ElectricProps& p = props_[c.material];
            if (p.role != ElectricRole::LampLit || p.toggle == kEmpty) continue;
            c.material = p.toggle;
            wakeAt(x, y, true);
            ++stats_.toggled;
        }
    }
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
    return it->second.v[static_cast<std::size_t>(((y & kChunkMask) << kChunkShift) | (x & kChunkMask))];
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
    for (float v : it->second.v) count += v != 0.0f;
    w.u32(count);
    for (int l = 0; l < kChunkCells; ++l) {
        const float v = it->second.v[static_cast<std::size_t>(l)];
        if (v == 0.0f) continue;
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
