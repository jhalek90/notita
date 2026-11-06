#include "Simulator.h"

#include "Rng.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace notita {

namespace {

constexpr float kHeatSleepEps = 0.01f;    // degrees per tick below which heat counts as settled
constexpr float kAmbientDeadband = 0.05f; // air this close to ambient is left alone

// Shared, read-only during a tick.
struct TickContext {
    const MaterialRegistry* mats;
    const SimParams* params;
    const ThermalProps* thermal;
    const PairConduct* pairs;
    std::size_t matCount;
    const ReactionTable* reactions; // null when off
    bool gasHook;
};

// Everything one chunk update needs. Lives on the worker's stack.
class ChunkUpdate {
public:
    ChunkUpdate(World& world, const TickContext& ctx, Chunk& self, std::uint64_t seed, std::uint16_t stamp)
        : mats_(*ctx.mats), ctx_(ctx), self_(self), rng_(seed), stamp_(stamp), params_(*ctx.params) {
        ox_ = self.originX();
        oy_ = self.originY();
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                nb_[dy + 1][dx + 1] = world.chunk(self.coord.x + dx, self.coord.y + dy);
    }

    std::uint32_t run() {
        const DirtyRect r = self_.rect;
        const ThermalProps* tp = ctx_.thermal;
        for (int ly = r.y1; ly >= r.y0; --ly) {        // bottom to top: falling stuff moves once
            const bool leftToRight = rng_.coin();       // alternate scan direction: no drift bias
            for (int i = r.x0; i <= r.x1; ++i) {
                const int lx = leftToRight ? i : r.x1 - (i - r.x0);
                Cell& c = self_.at(lx, ly);
                if (c.stamp == stamp_) continue;
                if (tp[c.material].reactive && ctx_.reactions && tryReact(ox_ + lx, oy_ + ly, &c)) continue;
                const Material& m = mats_[c.material];
                switch (m.kind) {
                case MaterialKind::Powder: ++processed_; updatePowder(ox_ + lx, oy_ + ly, &c, m); break;
                case MaterialKind::Liquid: ++processed_; updateLiquid(ox_ + lx, oy_ + ly, &c, m); break;
                case MaterialKind::Gas:    ++processed_; updateGas(ox_ + lx, oy_ + ly, &c, m); break;
                case MaterialKind::Special: ++processed_; updateFlame(ox_ + lx, oy_ + ly, &c); break;
                default: break;
                }
            }
        }
        if (params_.heat && self_.busy()) heat();
        if (!selfNext_.empty()) self_.nextRect.include(selfNext_.x0, selfNext_.y0, selfNext_.x1, selfNext_.y1);
        self_.events = events_;
        return processed_;
    }

private:
    // ---- neighbourhood access (own chunk + the 8 around it) ----
    Cell* at(int x, int y) {
        const int lx = x - ox_, ly = y - oy_;
        if (static_cast<unsigned>(lx) < static_cast<unsigned>(kChunkSize) &&
            static_cast<unsigned>(ly) < static_cast<unsigned>(kChunkSize))
            return &self_.cells[(ly << kChunkShift) | lx];
        const int ix = (lx >> kChunkShift) + 1, iy = (ly >> kChunkShift) + 1;
        if (static_cast<unsigned>(ix) > 2u || static_cast<unsigned>(iy) > 2u) return nullptr;
        Chunk* c = nb_[iy][ix];
        return c ? &c->cells[((ly & kChunkMask) << kChunkShift) | (lx & kChunkMask)] : nullptr;
    }

    void includeSelf(int x0, int y0, int x1, int y1) {
        selfNext_.x0 = std::min(selfNext_.x0, x0); selfNext_.y0 = std::min(selfNext_.y0, y0);
        selfNext_.x1 = std::max(selfNext_.x1, x1); selfNext_.y1 = std::max(selfNext_.y1, y1);
    }

    // Mark (x, y) and its 8 neighbours for the next tick.
    void wake(int x, int y) {
        const int lx = x - ox_, ly = y - oy_;
        if (lx >= 1 && lx <= kChunkSize - 2 && ly >= 1 && ly <= kChunkSize - 2) {
            includeSelf(lx - 1, ly - 1, lx + 1, ly + 1);
            return;
        }
        for (int iy = (ly - 1) >> kChunkShift; iy <= (ly + 1) >> kChunkShift; ++iy)
            for (int ix = (lx - 1) >> kChunkShift; ix <= (lx + 1) >> kChunkShift; ++ix) {
                if (ix < -1 || ix > 1 || iy < -1 || iy > 1) continue;
                const int bx = ix * kChunkSize, by = iy * kChunkSize;
                const int x0 = std::max(lx - 1 - bx, 0), x1 = std::min(lx + 1 - bx, kChunkMask);
                const int y0 = std::max(ly - 1 - by, 0), y1 = std::min(ly + 1 - by, kChunkMask);
                if (ix == 0 && iy == 0) includeSelf(x0, y0, x1, y1);
                else if (Chunk* c = nb_[iy + 1][ix + 1]) c->nextRect.include(x0, y0, x1, y1);
            }
    }

    void swapCells(Cell* a, int ax, int ay, Cell* b, int bx, int by) {
        std::swap(*a, *b);
        a->stamp = stamp_;
        b->stamp = stamp_;
        wake(ax, ay);
        wake(bx, by);
    }

    // Can a pixel of material m move into a cell holding t, moving vertically by dy?
    static bool canDisplace(const Material& m, const Material& t, int dy) {
        // a heavy liquid percolates down through a lighter powder (sand floats on quickmetal)
        if (t.kind == MaterialKind::Powder)
            return dy > 0 && m.kind == MaterialKind::Liquid && t.density < m.density;
        if (!t.isFluid()) return false;
        if (m.kind == MaterialKind::Gas) {
            if (t.kind == MaterialKind::Liquid) return false;
            if (dy < 0) return t.density > m.density;
            if (dy > 0) return t.density < m.density;
            return t.kind == MaterialKind::Empty;
        }
        if (dy > 0) return t.density < m.density;
        if (dy == 0) return t.kind != MaterialKind::Liquid;
        return false;
    }

    // Chance to sink one pixel into a lighter liquid this tick (drag / sorting speed).
    float sinkChance(const Material& m, const Material& t) const {
        if (t.kind == MaterialKind::Powder) return 0.25f; // squeezing through grains is slow
        if (t.kind != MaterialKind::Liquid) return 1.0f;
        const float diff = (m.density - t.density) / m.density;
        return std::clamp(diff * 3.0f, 0.05f, 1.0f) * (1.0f - 0.8f * t.viscosity);
    }

    // Fall straight down up to vy pixels. Returns the cell's new position
    // through (x, y, c) and whether it moved.
    bool fall(int& x, int& y, Cell*& c, const Material& m) {
        int vy = c->vy + static_cast<int>(params_.gravity * m.gravity);
        vy = std::clamp(vy, 0, m.maxFall * kVelOne);
        int steps = vy >> 8;
        if (static_cast<int>(rng_.below(kVelOne)) < (vy & (kVelOne - 1))) ++steps;
        steps = std::max(steps, 1);

        // A resting grain does not drop into a hole its lower neighbour left
        // this very tick: otherwise a wall of powder only erodes one grain per
        // tick from its base (the whole face just shuffles down) and stands
        // like a bell instead of collapsing. Falling grains still stream.
        const bool resting = c->vy == 0 && m.kind == MaterialKind::Powder;

        bool moved = false, inLiquid = false;
        for (int s = 0; s < steps; ++s) {
            Cell* below = at(x, y + 1);
            if (!below) break;
            const Material& t = mats_[below->material];
            if (!canDisplace(m, t, 1)) break;
            if (resting && s == 0 && below->stamp == stamp_) { wake(x, y); break; }
            if (t.kind == MaterialKind::Liquid || t.kind == MaterialKind::Powder) {
                inLiquid = true;
                if (rng_.unit() >= sinkChance(m, t)) { wake(x, y); break; }
            }
            swapCells(c, x, y, below, x, y + 1);
            c = below;
            ++y;
            moved = true;
        }
        // drag in liquids keeps sinking slow; otherwise keep the momentum
        c->vy = static_cast<std::int16_t>(moved ? (inLiquid ? std::min(vy, kVelOne / 2) : vy) : 0);
        return moved;
    }


    void updatePowder(int x, int y, Cell* c, const Material& m) {
        if (fall(x, y, c, m)) return;
        // resting: maybe slide off the pile
        int d = rng_.coin() ? 1 : -1;
        bool open = false;
        for (int k = 0; k < 2; ++k, d = -d) {
            Cell* diag = at(x + d, y + 1);
            Cell* side = at(x + d, y);
            if (!diag || !side || !canDisplace(m, mats_[diag->material], 1) || !mats_[side->material].isFluid()) continue;
            open = true;
            const float chance = mats_[diag->material].kind == MaterialKind::Liquid
                                     ? m.slide * sinkChance(m, mats_[diag->material]) : m.slide;
            if (rng_.unit() < chance) {
                swapCells(c, x, y, diag, x + d, y + 1);
                return;
            }
        }
        if (open) wake(x, y); // could still slide: stay awake
    }

    void updateLiquid(int x, int y, Cell* c, const Material& m) {
        if (fall(x, y, c, m)) return;

        int d = rng_.coin() ? 1 : -1;
        for (int k = 0; k < 2; ++k, d = -d) {
            Cell* diag = at(x + d, y + 1);
            Cell* side = at(x + d, y);
            if (diag && side && canDisplace(m, mats_[side->material], 0) &&
                canDisplace(m, mats_[diag->material], 1) &&
                rng_.unit() < sinkChance(m, mats_[diag->material])) {
                swapCells(c, x, y, diag, x + d, y + 1);
                return;
            }
        }

        if (m.spread <= 0) return;
        if (m.viscosity > 0.0f && rng_.unit() < m.viscosity) {
            // too thick to move this tick; stay awake if there is somewhere to go
            auto canGo = [&](const Cell* n) {
                if (!n) return false;
                const Material& nm = mats_[n->material];
                return canDisplace(m, nm, 0) || (nm.kind == MaterialKind::Liquid && nm.density < m.density);
            };
            if (canGo(at(x - 1, y)) || canGo(at(x + 1, y))) wake(x, y);
            return;
        }
        for (int k = 0; k < 2; ++k, d = -d) {
            int dist = 0;
            for (int i = 1; i <= m.spread; ++i) {
                Cell* t = at(x + d * i, y);
                if (!t || !canDisplace(m, mats_[t->material], 0)) break;
                dist = i;
                Cell* under = at(x + d * i, y + 1);
                if (under && canDisplace(m, mats_[under->material], 1)) break; // found a drop
            }
            if (dist > 0) {
                swapCells(c, x, y, at(x + d * dist, y), x + d * dist, y);
                return;
            }
        }
        // nowhere open: a denser liquid pushes sideways under a lighter
        // neighbour (water slides under oil instead of standing beside it)
        for (int k = 0; k < 2; ++k, d = -d) {
            Cell* t = at(x + d, y);
            if (!t) continue;
            const Material& tm = mats_[t->material];
            if (tm.kind != MaterialKind::Liquid || tm.density >= m.density) continue;
            if (rng_.unit() < sinkChance(m, tm)) {
                swapCells(c, x, y, t, x + d, y);
                return;
            }
            wake(x, y);
        }
    }

    void updateGas(int x, int y, Cell* c, const Material& m) {
        const int dir = m.density < mats_[kEmpty].density ? -1 : 1;
        const int dx = static_cast<int>(rng_.below(3)) - 1;
        // drift diagonally or straight along the buoyancy direction
        const int tries[2] = {dx, 0};
        for (int k = 0; k < (dx != 0 ? 2 : 1); ++k) {
            Cell* t = at(x + tries[k], y + dir);
            if (t && canDisplace(m, mats_[t->material], dir)) {
                swapCells(c, x, y, t, x + tries[k], y + dir);
                return;
            }
        }
        // blocked: spread sideways under a ceiling (or over a floor)
        if (m.spread <= 0) return;
        int d = rng_.coin() ? 1 : -1;
        for (int k = 0; k < 2; ++k, d = -d) {
            const int dist = 1 + static_cast<int>(rng_.below(static_cast<std::uint32_t>(m.spread)));
            int reach = 0;
            for (int i = 1; i <= dist; ++i) {
                Cell* t = at(x + d * i, y);
                if (!t || !canDisplace(m, mats_[t->material], 0)) break;
                reach = i;
            }
            if (reach > 0) {
                swapCells(c, x, y, at(x + d * reach, y), x + d * reach, y);
                return;
            }
        }
    }

    // ---------------------------------------------------------------- fire

    std::uint8_t flameLife(const ThermalProps& f) {
        return static_cast<std::uint8_t>(f.lifeMin + static_cast<int>(rng_.below(static_cast<std::uint32_t>(f.lifeMax - f.lifeMin + 1))));
    }

    void updateFlame(int x, int y, Cell* c) {
        const ThermalProps& p = ctx_.thermal[c->material];
        if (c->life == 0) {
            c->life = flameLife(p); // placed by a brush: start its clock
        } else if (--c->life == 0) {
            // burnt out: the heat stays behind as hot air
            c->material = kEmpty;
            c->flags = 0;
            c->stamp = stamp_;
            wake(x, y);
            return;
        }
        // a flame is the visible part of combustion: it stays at flame temperature while alive
        c->temperature = std::max(c->temperature, p.spawnTemp);
        c->stamp = stamp_;
        wake(x, y);
        // flicker upwards through air
        const int dx = static_cast<int>(rng_.below(3)) - 1;
        const int tries[2] = {dx, 0};
        for (int k = 0; k < (dx != 0 ? 2 : 1); ++k) {
            Cell* t = at(x + tries[k], y - 1);
            if (t && t->material == kEmpty) {
                swapCells(c, x, y, t, x + tries[k], y - 1);
                return;
            }
        }
    }

    // A neighbour fire can draw through: air, a flame, any gas (smoke drifting
    // off doesn't smother it) or a burning cell (the fire front eats into a
    // block whose surface is alight). Up first, then the sides, then down.
    Cell* findAir(int x, int y, int& ax, int& ay) {
        const int side = rng_.coin() ? 1 : -1;
        const int dx[4] = {0, side, -side, 0}, dy[4] = {-1, 0, 0, 1};
        for (int k = 0; k < 4; ++k) {
            Cell* n = at(x + dx[k], y + dy[k]);
            if (!n) continue;
            const MaterialKind kind = mats_[n->material].kind;
            if (kind == MaterialKind::Empty || kind == MaterialKind::Special || kind == MaterialKind::Gas ||
                (n->flags & kFlagBurning)) {
                ax = x + dx[k];
                ay = y + dy[k];
                return n;
            }
        }
        return nullptr;
    }

    void spawnFlame(Cell* n, int x, int y, MaterialId flame) {
        const ThermalProps& f = ctx_.thermal[flame];
        n->material = flame;
        n->flags = 0;
        n->vx = n->vy = 0;
        n->temperature = std::max(n->temperature, f.spawnTemp);
        n->life = flameLife(f);
        n->shade = static_cast<std::uint8_t>(rng_.below(256));
        n->stamp = stamp_;
        wake(x, y);
    }

    void burn(int x, int y, Cell& c, const ThermalProps& p) {
        int ax = 0, ay = 0;
        Cell* air = findAir(x, y, ax, ay);
        if (!air) {
            // nothing to draw through: smothered after a while
            if (rng_.below(64) == 0) c.flags &= static_cast<std::uint8_t>(~kFlagBurning);
            return;
        }
        if (c.temperature < p.burnMax) c.temperature = std::min(c.temperature + p.burnDT, p.burnMax);
        if (p.flame != kEmpty && air->material == kEmpty && rng_.chance16(p.flame16)) spawnFlame(air, ax, ay, p.flame);
        if (c.temperature < p.ignite - 80.0f) { // doused
            c.flags &= static_cast<std::uint8_t>(~kFlagBurning);
            c.life = 0;
            return;
        }
        if (c.life > 1) { --c.life; return; }
        // burnt out
        ++events_;
        c.material = (p.residue != kEmpty && rng_.chance16(p.residue16)) ? p.residue : p.burnsInto;
        c.flags = 0;
        c.life = 0;
        c.stamp = stamp_;
        handOverGas(x, y, c);
        wake(x, y);
    }

    // ------------------------------------------------------------ reactions

    bool tryReact(int x, int y, Cell* c) {
        const auto& list = ctx_.reactions->of(c->material);
        const auto& all = ctx_.reactions->reactions();
        static constexpr int kDx[4] = {0, 1, 0, -1}, kDy[4] = {-1, 0, 1, 0};
        const int start = static_cast<int>(rng_.below(4));
        bool pending = false;
        for (int k = 0; k < 4; ++k) {
            const int d = (start + k) & 3;
            const int nx = x + kDx[d], ny = y + kDy[d];
            Cell* n = at(nx, ny);
            if (!n || n->stamp == stamp_) continue;
            for (const ReactionTable::Entry& e : list) {
                if (e.partner != n->material) continue;
                const Reaction& r = all[e.index];
                const float ta = e.swapped ? n->temperature : c->temperature;
                if (ta < r.minTemp || ta > r.maxTemp) break;
                pending = true;
                if (rng_.unit() >= r.chance) break;
                if (e.swapped) react(r, n, nx, ny, c, x, y);
                else react(r, c, x, y, n, nx, ny);
                return true;
            }
        }
        if (pending) wake(x, y); // a partner is there: keep trying next tick
        return false;
    }

    void react(const Reaction& r, Cell* a, int ax, int ay, Cell* b, int bx, int by) {
        const ThermalProps* tp = ctx_.thermal;
        const float amb = params_.ambientTemp;
        // the pair's heat (relative to ambient) plus the reaction heat, shared by the products
        const float e = tp[a->material].cap * (a->temperature - amb) + tp[b->material].cap * (b->temperature - amb) + r.heat;
        const float t = std::clamp(amb + e / (tp[r.outA].cap + tp[r.outB].cap), kMinTemp, kMaxTemp);
        auto become = [&](Cell* cell, MaterialId m, int x, int y) {
            cell->material = m;
            cell->temperature = t;
            cell->flags = 0;
            cell->life = 0;
            cell->vx = cell->vy = 0;
            cell->stamp = stamp_;
            handOverGas(x, y, *cell);
            wake(x, y);
        };
        become(a, r.outA, ax, ay);
        become(b, r.outB, bx, by);
        ++events_;
    }

    // A freshly created gas goes to the gas grid instead, when one is attached.
    void handOverGas(int x, int y, Cell& c) {
        if (!ctx_.gasHook || !ctx_.thermal[c.material].takesGas) return;
        self_.gasOut.push_back({x, y, c.material, mats_[c.material].mass, c.temperature});
        c.material = kEmpty;
        c.flags = 0;
        c.life = 0;
    }

    // ----------------------------------------------------------------- heat

    // Temperature-driven change. The heat above (or below) the change point
    // is kept, the latent heat is taken (latent < 0) or given back (latent > 0).
    void changeState(int x, int y, Cell& c, MaterialId into, float point, float latent) {
        const ThermalProps* tp = ctx_.thermal;
        const float sensible = (c.temperature - point) * tp[c.material].cap;
        c.temperature = std::clamp(point + (sensible + latent) * tp[into].invCap, kMinTemp, kMaxTemp);
        c.material = into;
        c.flags = 0;
        c.life = 0;
        c.stamp = stamp_;
        ++events_;
        handOverGas(x, y, c);
        wake(x, y);
    }

    void cellEvents(int x, int y, Cell& c, const ThermalProps& p, bool& keep) {
        if (c.flags & kFlagBurning) {
            keep = true;
            burn(x, y, c, p);
            return;
        }
        const float t = c.temperature;
        if (t >= p.hotPoint) {
            keep = true;
            if (rng_.chance16(p.change16)) changeState(x, y, c, p.hotInto, p.hotPoint, -p.hotLatent);
            return;
        }
        if (t <= p.coldPoint) {
            keep = true;
            if (rng_.chance16(p.change16)) changeState(x, y, c, p.coldInto, p.coldPoint, p.coldLatent);
            return;
        }
        if (t >= p.ignite) {
            int ax, ay;
            if (findAir(x, y, ax, ay)) {
                c.flags |= kFlagBurning;
                c.life = static_cast<std::uint8_t>(p.burnTime);
                keep = true;
                ++events_;
            }
            return;
        }
        if (p.wakeForReaction && t >= p.reactMin) wake(x, y); // let the motion pass look for a partner
    }

    void heat() {
        const ThermalProps* tp = ctx_.thermal;
        const PairConduct* pc = ctx_.pairs;
        const std::size_t n = ctx_.matCount;
        Chunk* right = nb_[1][2];
        Chunk* down = nb_[2][1];
        const float amb = params_.ambientTemp, ambRate = params_.ambientRate;
        const float airCap = tp[kEmpty].cap;
        float maxChange = 0.0f;
        bool keep = false, wakeRight = false, wakeDown = false;
        double ambientOut = 0.0;
        const bool reverse = (stamp_ & 1u) != 0; // alternate the scan so heat has no drift direction

        auto exchange = [&](Cell& a, Cell& b, bool* wakeOther) {
            const float d = a.temperature - b.temperature;
            if (d == 0.0f) return;
            const PairConduct& g = pc[a.material * n + b.material];
            const float da = g.a * d, db = g.b * d;
            a.temperature -= da;
            b.temperature += db;
            maxChange = std::max(maxChange, std::abs(da));
            if (wakeOther) { if (std::abs(db) > kHeatSleepEps) *wakeOther = true; }
            else maxChange = std::max(maxChange, std::abs(db));
        };

        for (int i = 0; i < kChunkCells; ++i) {
            const int idx = reverse ? kChunkCells - 1 - i : i;
            const int lx = idx & kChunkMask, ly = idx >> kChunkShift;
            Cell& c = self_.cells[idx];
            if (lx < kChunkMask) exchange(c, self_.cells[idx + 1], nullptr);
            else if (right) exchange(c, right->cells[ly << kChunkShift], &wakeRight);
            if (ly < kChunkMask) exchange(c, self_.cells[idx + kChunkSize], nullptr);
            else if (down) exchange(c, down->cells[lx], &wakeDown);

            const ThermalProps& p = tp[c.material];
            if (p.events) cellEvents(ox_ + lx, oy_ + ly, c, p, keep);
            if (c.material == kEmpty && ambRate > 0.0f) {
                const float dev = amb - c.temperature;
                if (std::abs(dev) > kAmbientDeadband) {
                    const float dt = dev * ambRate;
                    c.temperature += dt;
                    ambientOut -= static_cast<double>(dt) * airCap;
                    maxChange = std::max(maxChange, std::abs(dt));
                }
            }
        }

        // pairs across the left and top edges belong to those neighbours: wake them when it matters
        auto edge = [&](Chunk* other, bool vertical) {
            if (!other) return;
            for (int k = 0; k < kChunkSize; ++k) {
                const Cell& mine = vertical ? self_.cells[k << kChunkShift] : self_.cells[k];
                const Cell& theirs = vertical ? other->cells[(k << kChunkShift) | kChunkMask]
                                              : other->cells[(kChunkMask << kChunkShift) | k];
                const float d = mine.temperature - theirs.temperature;
                if (d == 0.0f) continue;
                const PairConduct& g = pc[theirs.material * n + mine.material];
                if (std::abs(g.a * d) > kHeatSleepEps || std::abs(g.b * d) > kHeatSleepEps) {
                    other->heatNext.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        };
        edge(nb_[1][0], true);
        edge(nb_[0][1], false);

        if (wakeRight) right->heatNext.store(true, std::memory_order_relaxed);
        if (wakeDown) down->heatNext.store(true, std::memory_order_relaxed);
        if (keep || maxChange > kHeatSleepEps) self_.heatNext.store(true, std::memory_order_relaxed);
        self_.ambientEnergy += ambientOut;
    }

    const MaterialRegistry& mats_;
    const TickContext& ctx_;
    Chunk& self_;
    Chunk* nb_[3][3]{};
    int ox_ = 0, oy_ = 0;
    Rng rng_;
    std::uint16_t stamp_;
    const SimParams& params_;
    DirtyRect selfNext_;
    std::uint32_t processed_ = 0;
    std::uint32_t events_ = 0;
};

static_assert(16 + 1 + 1 < kMaxReach, "max fall / spread must stay inside the checkerboard margin");

} // namespace

Simulator::Simulator(World& world, const MaterialRegistry& materials, ThreadPool* pool, std::uint64_t seed,
                     const ReactionTable* reactions)
    : world_(world), mats_(materials), pool_(pool), seed_(seed), reactions_(reactions) {
    rebuildTables();
}

void Simulator::setReactions(const ReactionTable* reactions) {
    reactions_ = reactions;
    rebuildTables();
}

void Simulator::setGasExchange(GasExchange* exchange) {
    gas_ = exchange;
    rebuildTables();
}

void Simulator::rebuildTables() {
    const std::size_t n = mats_.size();
    thermal_.assign(n, ThermalProps{});
    auto chance16 = [](float p) { return static_cast<std::uint32_t>(std::clamp(p, 0.0f, 1.0f) * 65536.0f); };
    for (std::size_t i = 0; i < n; ++i) {
        const Material& m = mats_[static_cast<MaterialId>(i)];
        ThermalProps& p = thermal_[i];
        p.cap = m.heatCapacity;
        p.invCap = 1.0f / m.heatCapacity;
        p.spawnTemp = m.spawnTemp;
        p.lifeMin = m.lifeMin;
        p.lifeMax = m.lifeMax;
        p.change16 = chance16(m.changeChance);
        if (m.hot.valid) {
            p.hotPoint = m.hot.point;
            p.hotInto = m.hot.into;
            p.hotLatent = m.latentHeat;
        }
        if (m.cold.valid) {
            p.coldPoint = m.cold.point;
            p.coldInto = m.cold.into;
            // going back down gives back the latent heat the other side took on the way up
            const Material& into = mats_[m.cold.into];
            p.coldLatent = (into.hot.valid && into.hot.into == m.id) ? into.latentHeat : 0.0f;
        }
        if (m.flammable()) {
            p.ignite = m.ignitePoint;
            p.burnTime = std::max(1, m.burnTime);
            p.burnDT = m.burnHeat / m.heatCapacity;
            p.burnMax = m.burnMaxTemp;
            p.flame16 = chance16(m.flameChance);
            p.burnsInto = m.burnsInto.into;
            p.flame = m.flame;
            if (m.burnResidue.valid) {
                p.residue = m.burnResidue.into;
                p.residue16 = chance16(m.residueChance);
            }
        }
        p.events = m.hot.valid || m.cold.valid || m.flammable();
        if (reactions_ && reactions_->reactive(static_cast<MaterialId>(i))) {
            p.reactive = true;
            p.reactMin = reactions_->lowestMinTemp(static_cast<MaterialId>(i));
            p.wakeForReaction = p.reactMin > -kNoPoint;
            p.events = p.events || p.wakeForReaction;
        }
        p.takesGas = gas_ && m.kind == MaterialKind::Gas && gas_->takesGas(m.id);
    }

    // Pair conduction. g is the series (harmonic mean) conductance, capped
    // so one exchange moves the two cells at most halfway to their common
    // temperature. Every exchange is then a convex average of the pair, so the
    // in-place, pair-by-pair update can't overshoot or blow up, and
    // a*C1 == b*C2 keeps the energy.
    const float k = std::clamp(params_.conduction, 0.0f, 1.0f) * 0.5f;
    pairs_.assign(n * n, PairConduct{});
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < n; ++j) {
            const Material& a = mats_[static_cast<MaterialId>(i)];
            const Material& b = mats_[static_cast<MaterialId>(j)];
            const float sum = a.conductivity + b.conductivity;
            float g = sum > 0.0f ? k * 2.0f * a.conductivity * b.conductivity / sum : 0.0f;
            const float ia = 1.0f / a.heatCapacity, ib = 1.0f / b.heatCapacity;
            g = std::min(g, 0.5f / (ia + ib));
            pairs_[i * n + j] = {g * ia, g * ib};
        }
    tablesConduction_ = params_.conduction;
}

void Simulator::updateChunk(Chunk& chunk, int pass) {
    const std::uint64_t s = Rng::hash(Rng::hash(seed_, tick_ * 4 + static_cast<std::uint64_t>(pass)),
                                      chunkKey(chunk.coord.x, chunk.coord.y));
    const auto stamp = static_cast<std::uint16_t>(tick_ % 65535u + 1u); // never 0 (fresh cells)
    const TickContext ctx{&mats_, &params_, thermal_.data(), pairs_.data(), mats_.size(),
                          params_.reactions ? reactions_ : nullptr, gas_ != nullptr};
    ChunkUpdate u(world_, ctx, chunk, s, stamp);
    chunk.cellsProcessed = u.run();
}

void Simulator::tick() {
    const auto t0 = std::chrono::steady_clock::now();
    if (params_.conduction != tablesConduction_) rebuildTables();
    ++tick_;
    stats_.activeChunks = world_.beginTick();
    stats_.totalChunks = static_cast<int>(world_.chunks().size());

    int heatChunks = 0;
    if (params_.heat)
        for (const Chunk* c : world_.chunks()) heatChunks += c->busy();
    stats_.heatChunks = heatChunks;

    for (int pass = 0; pass < 4; ++pass) {
        const int px = pass & 1, py = pass >> 1;
        jobs_.clear();
        for (Chunk* c : world_.chunks())
            if ((c->active() || (params_.heat && c->heatActive)) && (c->coord.x & 1) == px && (c->coord.y & 1) == py)
                jobs_.push_back(c);
        if (jobs_.empty()) continue;
        const std::function<void(int)> job = [&](int i) { updateChunk(*jobs_[i], pass); };
        if (pool_) pool_->parallelFor(static_cast<int>(jobs_.size()), job);
        else for (int i = 0; i < static_cast<int>(jobs_.size()); ++i) job(i);
    }

    std::uint64_t processed = 0, events = 0;
    double ambient = 0.0;
    emissions_.clear();
    for (Chunk* c : world_.chunks()) {
        processed += c->cellsProcessed;
        events += c->events;
        ambient += c->ambientEnergy;
        if (!c->gasOut.empty()) {
            emissions_.insert(emissions_.end(), c->gasOut.begin(), c->gasOut.end());
            c->gasOut.clear();
        }
    }
    if (gas_) {
        gas_->absorb(emissions_.data(), emissions_.size());
        gas_->ambientHeat(ambient);
    }
    stats_.cellsProcessed = processed;
    stats_.events = events;
    stats_.tick = tick_;
    stats_.tickMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace notita
