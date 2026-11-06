#include "Simulator.h"

#include "Bodies.h" // kAuxSettled

#include "Particles.h"
#include "Rng.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace notita {

namespace {

constexpr float kHeatSleepEps = 0.01f;    // degrees per tick below which heat counts as settled
constexpr float kAmbientDeadband = 0.05f; // air this close to ambient is left alone
constexpr float kMinHeatDiff = 1.0e-3f;   // neighbours closer than this don't exchange heat

// Shared, read-only during a tick.
struct TickContext {
    const MaterialRegistry* mats;
    const SimParams* params;
    const ThermalProps* thermal;
    const PairConduct* pairs;
    std::size_t matCount;
    const ReactionTable* reactions; // null when off
    const std::uint16_t* reactPairs; // [a * matCount + b]: reaction index + 1, or 0
    bool gasHook;
    const std::uint8_t* miscible;    // [a * matCount + b]: the two liquids blend
    bool anyMiscible;
    bool particles;                  // a ParticleSystem takes splashes
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
                updateCell(ox_ + lx, oy_ + ly, &c, tp);
            }
        }
        if (params_.heat && self_.busy()) heat();
        if (!selfNext_.empty()) self_.nextRect.include(selfNext_.x0, selfNext_.y0, selfNext_.x1, selfNext_.y1);
        self_.events = events_;
        return processed_;
    }

private:
    void updateCell(int x, int y, Cell* c, const ThermalProps* tp) {
        if (tp[c->material].reactive && ctx_.reactions && tryReact(x, y, c)) return;
        const Material& m = mats_[c->material];
        switch (m.kind) {
        case MaterialKind::Powder: ++processed_; updatePowder(x, y, c, m); break;
        case MaterialKind::Liquid: ++processed_; updateLiquid(x, y, c, m); break;
        case MaterialKind::Gas:    ++processed_; updateGas(x, y, c, m); break;
        case MaterialKind::Special: ++processed_; updateFlame(x, y, c); break;
        default: break;
        }
    }

    // #477 (smoothPowder): the cell under a falling cell lies in the chunk below,
    // whose pass comes later this tick (even chunk rows run before odd ones), and
    // is still falling itself. Treating it as a floor made everything above the
    // border lag a tick behind, so a stream showed a gap (and liquids a stall) at
    // every other chunk border. Update it now instead; it takes this tick's stamp,
    // so its own chunk skips it. Only cells in the top 15 rows of the chunk below
    // (its whole update stays within kMaxReach of this chunk), and recursion is
    // bounded, so the threading contract and determinism hold.
    bool advance(int x, int y) {
        if (!params_.smoothPowder || advanceDepth_ >= 16 || (self_.coord.y & 1) != 0) return false;
        const int ly = y - oy_;
        if (ly < kChunkSize || ly > kChunkSize + 14) return false;
        Cell* t = at(x, y);
        if (!t || t->stamp == stamp_ || (t->flags & kFlagBody) || t->vy <= 0) return false;
        const MaterialKind k = mats_[t->material].kind;
        if (k != MaterialKind::Powder && k != MaterialKind::Liquid) return false;
        const bool q = queued_;
        ++advanceDepth_;
        updateCell(x, y, t, ctx_.thermal);
        --advanceDepth_;
        queued_ = q;
        return t->stamp == stamp_;
    }

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

    bool miscible(const Material& a, const Material& b) const {
        return ctx_.anyMiscible && ctx_.miscible[a.id * ctx_.matCount + b.id] != 0;
    }

    // Can a pixel of material m move into a cell holding t, moving vertically by dy?
    bool canDisplace(const Material& m, const Material& t, int dy) const {
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
        // miscible liquids blend by diffusion (updateLiquid) instead of layering
        if (dy > 0) return t.density < m.density && !(t.kind == MaterialKind::Liquid && miscible(m, t));
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
        if (params_.smoothPowder && m.kind == MaterialKind::Powder) return fallGrain(x, y, c, m);
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

        bool moved = false, inLiquid = false, queuedLiquid = false;
        for (int s = 0; s < steps; ++s) {
            Cell* below = at(x, y + 1);
            if (!below) break;
            const Material& t = mats_[below->material];
            if (!canDisplace(m, t, 1)) {
                if (advance(x, y + 1)) { --s; continue; } // the cell below moved on first: try again
                // queued behind a cell that is still falling: keep the speed (#477)
                if (params_.smoothPowder && below->vy > 0 && !(below->flags & kFlagBody) &&
                    (t.kind == MaterialKind::Liquid || t.kind == MaterialKind::Powder)) {
                    vy = std::min(vy, std::max(static_cast<int>(below->vy), vy * 3 / 4));
                    queuedLiquid = true;
                    wake(x, y);
                }
                break;
            }
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
        c->vy = static_cast<std::int16_t>(moved || queuedLiquid ? (inLiquid ? std::min(vy, kVelOne / 2) : vy) : 0);
        // a liquid that fell this tick and was stopped hard splashes (phase 4)
        if (moved && !inLiquid && ctx_.particles && m.kind == MaterialKind::Liquid &&
            static_cast<float>(vy) >= params_.splashSpeed * kVelOne) {
            const Cell* below = at(x, y + 1);
            if (below && !(below->flags & kFlagBody) && !canDisplace(m, mats_[below->material], 1)) splash(x, y, c, m, vy, *below);
        }
        return moved;
    }

    // ------------------------------------------------------- smooth grains
    // (SimParams::smoothPowder, #477). The phase 1 rule stopped a grain dead
    // (vy = 0) whenever the cell under it was taken, even by a grain that was
    // itself still falling. A packed stream then stalled as a whole the tick
    // its front landed and restarted from rest, so it came down in steps; and
    // at every border where the upper chunk runs before the lower one (pass
    // order), a stream saw the lower chunk's not-yet-moved grains as a floor
    // and stalled there too. Here a grain queued behind a moving grain keeps
    // most of its speed, gravity has a little per-grain jitter (a falling
    // block stretches and breaks up), and a grain landing on resting ground
    // turns part of its fall into sideways speed (Cell::vx) that makes it
    // roll down the pile and slide a little at its foot.

    static constexpr int kGrainVxCap = 2 * kVelOne;   // fastest sideways roll
    static constexpr int kGrainVxStop = 24;           // slower than this counts as stopped

    bool movingGrain(const Cell& b) const {
        return mats_[b.material].kind == MaterialKind::Powder && (b.vy != 0 || b.vx != 0);
    }

    static int decay(int v, int num, int den) {
        v = v * num / den;
        return std::abs(v) < kGrainVxStop ? 0 : v;
    }

    bool fallGrain(int& x, int& y, Cell*& c, const Material& m) {
        queued_ = false;
        const int g = static_cast<int>(static_cast<float>(params_.gravity) * m.gravity);
        int vy = c->vy + g - g / 4 + static_cast<int>(rng_.below(static_cast<std::uint32_t>(g / 2 + 1)));
        vy = std::clamp(vy, 0, m.maxFall * kVelOne);
        int vx = c->vx;
        int steps = vy >> 8;
        if (static_cast<int>(rng_.below(kVelOne)) < (vy & (kVelOne - 1))) ++steps;
        steps = std::max(steps, 1);
        const bool resting = c->vy == 0 && c->vx == 0;

        bool moved = false, inLiquid = false;
        int impact = 0, side = 0;
        for (int s = 0; s < steps; ++s) {
            // sideways speed bends the fall into an arc (at most 2 px a tick)
            int dx = 0;
            if (vx != 0 && side < 2 && static_cast<int>(rng_.below(kVelOne)) < std::abs(vx)) {
                dx = vx > 0 ? 1 : -1;
                Cell* sc = at(x + dx, y);
                Cell* dc = at(x + dx, y + 1);
                if (!sc || !dc || !canDisplace(m, mats_[sc->material], 0) || !canDisplace(m, mats_[dc->material], 1)) dx = 0;
            }
            Cell* below = at(x + dx, y + 1);
            if (!below) break;
            const Material& t = mats_[below->material];
            if (!canDisplace(m, t, 1)) {
                if (advance(x + dx, y + 1)) { --s; continue; } // the cell below moved on first: try again
                if (movingGrain(*below)) {
                    // queued behind a grain that is still moving: keep most of the speed
                    vy = std::min(vy, std::max(static_cast<int>(below->vy), vy * 3 / 4));
                    queued_ = !moved;
                    wake(x, y);
                } else {
                    impact = vy;
                }
                break;
            }
            if (resting && s == 0 && below->stamp == stamp_) {
                // the phase 1 rule (a wall face slides off grain by grain instead of
                // shuffling down), unless the grain under it fell straight out: then
                // nothing holds this one up and it follows (a block drops as one)
                const Cell* gone = at(x, y + 2);
                if (!(gone && movingGrain(*gone) && gone->vy > 0 && gone->stamp == stamp_)) { wake(x, y); break; }
            }
            if (t.kind == MaterialKind::Liquid || t.kind == MaterialKind::Powder) {
                inLiquid = true;
                if (rng_.unit() >= sinkChance(m, t)) { wake(x, y); break; }
            }
            swapCells(c, x, y, below, x + dx, y + 1);
            c = below;
            x += dx;
            ++y;
            moved = true;
            if (dx != 0) ++side;
        }
        if (inLiquid) {
            vy = std::min(vy, kVelOne / 2);
            vx = decay(vx, 1, 2);
        } else if (impact > 0) {
            // landed on resting ground: part of the fall becomes a sideways roll,
            // towards an open side (a random one when both or neither are open)
            vy = 0;
            if (impact >= kVelOne / 2) {
                auto open = [&](int d) {
                    Cell* sc = at(x + d, y);
                    Cell* dc = at(x + d, y + 1);
                    return sc && dc && canDisplace(m, mats_[sc->material], 0) && canDisplace(m, mats_[dc->material], 1);
                };
                // only off a pile or an edge: on flat ground a grain stays where it lands
                const bool l = open(-1), r = open(1);
                if (l || r) {
                    int d = vx > 0 ? 1 : vx < 0 ? -1 : (rng_.coin() ? 1 : -1);
                    if (l != r) d = r ? 1 : -1;
                    const float k = (0.25f + 0.45f * rng_.unit()) * std::max(m.slide, 0.1f);
                    const int add = static_cast<int>(static_cast<float>(impact) * k);
                    vx = std::clamp(vx + d * add, -kGrainVxCap, kGrainVxCap);
                }
            }
            if (vx != 0) wake(x, y);
        } else if (!moved && !queued_) {
            vy = 0;
        }
        if (!inLiquid && moved && impact == 0) vx = decay(vx, 31, 32); // a little air drag
        c->vy = static_cast<std::int16_t>(vy);
        c->vx = static_cast<std::int16_t>(vx);
        return moved;
    }

    // A grain on the ground with sideways speed rolls down the slope or slides
    // on flat ground (friction halves it each step); without speed it falls
    // back to the phase 1 slide chance.
    bool rollGrain(int x, int y, Cell* c, const Material& m) {
        const int vx = c->vx;
        if (vx == 0) return false;
        const int d = vx > 0 ? 1 : -1;
        Cell* diag = at(x + d, y + 1);
        Cell* side = at(x + d, y);
        if (side && canDisplace(m, mats_[side->material], 0)) {
            if (diag && canDisplace(m, mats_[diag->material], 1)) {
                const bool wet = mats_[diag->material].kind == MaterialKind::Liquid;
                if (!wet || rng_.unit() < sinkChance(m, mats_[diag->material])) {
                    c->vx = static_cast<std::int16_t>(decay(vx, wet ? 1 : 3, wet ? 2 : 4));
                    swapCells(c, x, y, diag, x + d, y + 1);
                    return true;
                }
            } else if (static_cast<int>(rng_.below(kVelOne)) < std::abs(vx)) {
                c->vx = static_cast<std::int16_t>(decay(vx, 1, 2));
                swapCells(c, x, y, side, x + d, y);
                return true;
            }
            c->vx = static_cast<std::int16_t>(decay(vx, 3, 4));
        } else {
            c->vx = 0; // ran into something
        }
        wake(x, y);
        return c->vx != 0;
    }

    // ------------------------------------------------------------- splashes

    // (x, y) holds a liquid that just hit `below` at vy (fixed point). The
    // surface pixels of the pool around the impact (and, on hard ground, the
    // drop itself) fly off as particles. The ejecta share at most
    // `restitution` of the impact momentum: each gets a random share of the
    // budget until it is spent. Candidates stay within 3 px (inside kMaxReach).
    void splash(int x, int y, Cell* c, const Material& m, int vy, const Cell& below) {
        const float speed = static_cast<float>(vy) / kVelOne;
        const float impactP = speed * static_cast<float>(m.mass);
        float budget = impactP * params_.restitution;
        if (budget <= 0.0f) return;
        const bool pool = mats_[below.material].kind == MaterialKind::Liquid;
        const auto chance16 = static_cast<std::uint32_t>(params_.splashChance * 65536.0f);
        struct Cand { Cell* cell; int x, y, side; };
        Cand cands[7];
        int n = 0;
        for (int k = 1; k <= 3; ++k)
            for (int side = -1; side <= 1; side += 2) {
                Cell* s = at(x + side * k, y + 1);
                Cell* above = at(x + side * k, y);
                if (!s || !above || s->stamp == stamp_ || (s->flags & kFlagBody)) continue;
                if (mats_[s->material].kind != MaterialKind::Liquid || above->material != kEmpty) continue;
                cands[n++] = {s, x + side * k, y + 1, side};
            }
        if (!pool) cands[n++] = {c, x, y, 0}; // a drop on hard ground bursts sideways
        const int start = n > 1 ? static_cast<int>(rng_.below(static_cast<std::uint32_t>(n))) : 0;
        double out = 0.0;
        for (int q = 0; q < n && budget > 0.05f; ++q) {
            const Cand& cd = cands[(start + q) % n];
            if (!rng_.chance16(chance16)) continue;
            const Material& cm = mats_[cd.cell->material];
            const float cmass = static_cast<float>(std::max(cm.mass, 1));
            const float share = (0.25f + 0.5f * rng_.unit()) * budget;        // momentum handed to this one
            const float sp = std::min(share / cmass, params_.splashSpeed * 2.5f);
            budget -= sp * cmass;
            // up and outwards; the drop itself skids low and sideways
            const float a = cd.side == 0 ? (0.9f + 0.5f * rng_.unit()) : (0.25f + 0.8f * rng_.unit()); // radians from vertical
            const int side = cd.side != 0 ? cd.side : (rng_.coin() ? 1 : -1);
            Particle p;
            p.x = static_cast<float>(cd.x) + 0.5f;
            p.y = static_cast<float>(cd.y) + 0.5f;
            p.vx = static_cast<float>(side) * sp * std::sin(a);
            p.vy = -sp * std::cos(a);
            p.cell = *cd.cell;
            p.cell.stamp = 0;
            p.cell.flags = 0;
            p.cell.vx = p.cell.vy = 0;
            self_.particlesOut.push_back(p);
            out += static_cast<double>(sp) * cmass;
            *cd.cell = Cell{};
            cd.cell->temperature = params_.ambientTemp;
            cd.cell->stamp = stamp_;
            wake(cd.x, cd.y);
        }
        if (out > 0.0) {
            self_.splashIn += impactP;
            self_.splashOut += out;
            if (c->material == kEmpty) return; // the drop itself flew off
            c->vy = 0; // the rest of the momentum went into the pool / the ground
        }
    }

    // Miscible liquids interdiffuse: a random swap with a different miscible
    // neighbour. Cell::aux counts a pixel's swaps: it starts swaps while under
    // mixSteps and is taken along while under twice that, so every pixel's
    // mixing is bounded, a blended pool sleeps, and fresh liquid (aux 0) still
    // works its way into a pool that has settled.
    bool mix(int x, int y, Cell* c, const Material& m) {
        if (m.miscibleWith.empty() || c->aux >= params_.mixSteps) return false;
        static constexpr int kDx[4] = {1, -1, 0, 0}, kDy[4] = {0, 0, 1, -1};
        const int first = static_cast<int>(rng_.below(4));
        bool partner = false;
        for (int k = 0; k < 4; ++k) {
            const int d = (first + k) & 3;
            Cell* t = at(x + kDx[d], y + kDy[d]);
            if (!t || t->material == c->material || t->stamp == stamp_) continue;
            const Material& tm = mats_[t->material];
            if (tm.kind != MaterialKind::Liquid || !miscible(m, tm) || t->aux >= 2 * params_.mixSteps) continue;
            partner = true;
            if (rng_.unit() >= params_.mixRate) continue;
            if (c->aux < 255) ++c->aux;
            if (t->aux < 255) ++t->aux;
            swapCells(c, x, y, t, x + kDx[d], y + kDy[d]);
            return true;
        }
        if (partner) wake(x, y);
        return false;
    }


    void updatePowder(int x, int y, Cell* c, const Material& m) {
        if (fall(x, y, c, m)) return;
        if (params_.smoothPowder) {
            if (queued_) return;                    // waiting behind a moving grain
            if (rollGrain(x, y, c, m)) return;
        }
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

    static constexpr std::uint8_t kCalmRuns = 6;          // surface runs a resting liquid pixel may start
    static constexpr std::uint32_t kCalmStart16 = 65536 / 4; // chance per tick to start one

    void updateLiquid(int x, int y, Cell* c, const Material& m) {
        if (fall(x, y, c, m)) {
            if (params_.calmLiquid && !(c->flags & kFlagBurning) && c->material == m.id) c->life = 0;
            return;
        }
        if (c->material == kEmpty) return; // splashed away
        if (ctx_.anyMiscible && mix(x, y, c, m)) return;

        const bool calm = params_.calmLiquid;
        int d = rng_.coin() ? 1 : -1;
        if (calm && c->vx != 0) d = c->vx > 0 ? 1 : -1; // keep flowing the way it went
        for (int k = 0; k < 2; ++k, d = -d) {
            Cell* diag = at(x + d, y + 1);
            Cell* side = at(x + d, y);
            if (diag && side && canDisplace(m, mats_[side->material], 0) &&
                canDisplace(m, mats_[diag->material], 1) &&
                rng_.unit() < sinkChance(m, mats_[diag->material])) {
                if (calm) {
                    c->vx = static_cast<std::int16_t>(d * kVelOne);
                    if (!(c->flags & kFlagBurning)) c->life = 0;
                }
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
                return canDisplace(m, nm, 0) || (nm.kind == MaterialKind::Liquid && nm.density < m.density && !miscible(m, nm));
            };
            if (canGo(at(x - 1, y)) || canGo(at(x + 1, y))) wake(x, y);
            return;
        }
        for (int k = 0; k < 2; ++k, d = -d) {
            int dist = 0;
            bool drop = false;
            for (int i = 1; i <= m.spread; ++i) {
                Cell* t = at(x + d * i, y);
                if (!t || !canDisplace(m, mats_[t->material], 0)) break;
                dist = i;
                Cell* under = at(x + d * i, y + 1);
                if (under && canDisplace(m, mats_[under->material], 1)) { drop = true; break; } // found a drop
            }
            if (dist > 0) {
                if (calm) {
                    // Flat surface, no drop in reach (#477): the phase 1 rule shuffled a pool's
                    // partial top row back and forth every tick, for ever. Here a pixel keeps
                    // flowing the way it goes (Cell::vx) until something stops it, and may
                    // start such a run along the surface only kCalmRuns times (Cell::life,
                    // free on a liquid that isn't burning) before it rests. Falling or finding
                    // a drop resets the count, so 1 px steps still level out, then it sleeps.
                    const bool flowing = (c->vx > 0 && d > 0) || (c->vx < 0 && d < 0);
                    const bool burning = (c->flags & kFlagBurning) != 0;
                    if (drop) {
                        if (!burning) c->life = 0;
                    } else if (!flowing) {
                        if (burning || c->life >= kCalmRuns) continue; // rests in its row: lets the chunk sleep
                        if (!rng_.chance16(kCalmStart16)) { wake(x, y); continue; }
                        ++c->life;
                    }
                    int vx = drop ? d * kVelOne : (flowing ? c->vx * 15 / 16 : d * kVelOne);
                    if (std::abs(vx) < 24) vx = 0;
                    c->vx = static_cast<std::int16_t>(vx);
                }
                swapCells(c, x, y, at(x + d * dist, y), x + d * dist, y);
                return;
            }
        }
        if (calm) c->vx = 0;
        // nowhere open: a denser liquid pushes sideways under a lighter
        // neighbour (water slides under oil instead of standing beside it)
        for (int k = 0; k < 2; ++k, d = -d) {
            Cell* t = at(x + d, y);
            if (!t) continue;
            const Material& tm = mats_[t->material];
            if (tm.kind != MaterialKind::Liquid || tm.density >= m.density || miscible(m, tm)) continue;
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
        // cheap filter first: one table load per neighbour
        const std::uint16_t* pairRow = ctx_.reactPairs + static_cast<std::size_t>(c->material) * ctx_.matCount;
        static constexpr int kDx[4] = {0, 1, 0, -1}, kDy[4] = {-1, 0, 1, 0};
        Cell* hit[4];
        int hx[4], hy[4], hits = 0;
        for (int d = 0; d < 4; ++d) {
            Cell* n = at(x + kDx[d], y + kDy[d]);
            if (n && pairRow[n->material] != 0 && n->stamp != stamp_) {
                hit[hits] = n;
                hx[hits] = x + kDx[d];
                hy[hits] = y + kDy[d];
                ++hits;
            }
        }
        if (hits == 0) return false;
        const auto& all = ctx_.reactions->reactions();
        const int start = hits > 1 ? static_cast<int>(rng_.below(static_cast<std::uint32_t>(hits))) : 0;
        bool pending = false;
        for (int k = 0; k < hits; ++k) {
            const int i = (start + k) % hits;
            Cell* n = hit[i];
            const Reaction& r = all[pairRow[n->material] - 1u];
            const bool swapped = r.a != c->material;
            const float ta = swapped ? n->temperature : c->temperature;
            if (ta < r.minTemp || ta > r.maxTemp) continue;
            pending = true;
            if (rng_.unit() >= r.chance) continue;
            if (swapped) react(r, n, hx[i], hy[i], c, x, y);
            else react(r, c, x, y, n, hx[i], hy[i]);
            return true;
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
            if (mats_[m].kind == MaterialKind::Solid) cell->aux = kAuxSettled; // as for state changes
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
        // a solid that froze (or set) in place counts as put down by physics: it only
        // becomes a rigid body when nothing holds it up (a magma crust stays on its cave)
        if (mats_[into].kind == MaterialKind::Solid) c.aux = kAuxSettled;
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

        // Pairs closer than kMinHeatDiff don't exchange (settled areas stay put).
        // into the next chunk: wake it if its cell changed noticeably
        auto exchangeOut = [&](Cell& a, Cell& b, bool& wakeOther) {
            const float d = a.temperature - b.temperature;
            if (std::abs(d) < kMinHeatDiff) return;
            const PairConduct& g = pc[a.material * n + b.material];
            const float da = g.a * d, db = g.b * d;
            a.temperature -= da;
            b.temperature += db;
            maxChange = std::max(maxChange, std::abs(da));
            if (std::abs(db) > kHeatSleepEps) wakeOther = true;
        };

        // 1. conduction: each cell trades with its right and lower neighbour,
        //    rows top-down on even ticks and bottom-up on odd ones. Within a
        //    row the horizontal fluxes are all computed from the same values,
        //    then applied (no serial chain along the row); with the pair cap
        //    every cell still ends at a convex average, so it can't overshoot.
        //    Each pair moves a*C1 == b*C2 of energy, so the sum is kept exactly
        //    (up to float rounding).
        float leave[kChunkSize + 1], arrive[kChunkSize + 1]; // [x]: pair (x, x+1)
        for (int k = 0; k < kChunkSize; ++k) {
            const int ly = reverse ? kChunkMask - k : k;
            Cell* row = &self_.cells[ly << kChunkShift];
            float rowMax = 0.0f;
            for (int lx = 0; lx < kChunkMask; ++lx) {
                float d = row[lx].temperature - row[lx + 1].temperature;
                d = std::abs(d) < kMinHeatDiff ? 0.0f : d;
                const PairConduct& g = pc[row[lx].material * n + row[lx + 1].material];
                leave[lx] = g.a * d;
                arrive[lx + 1] = g.b * d;
                rowMax = std::max(rowMax, std::max(std::abs(leave[lx]), std::abs(arrive[lx + 1])));
            }
            leave[kChunkMask] = 0.0f;
            arrive[0] = 0.0f;
            for (int lx = 0; lx < kChunkSize; ++lx) row[lx].temperature += arrive[lx] - leave[lx];
            if (right) exchangeOut(row[kChunkMask], right->cells[ly << kChunkShift], wakeRight);
            if (ly < kChunkMask) {
                Cell* below = row + kChunkSize;
                for (int lx = 0; lx < kChunkSize; ++lx) {
                    float d = row[lx].temperature - below[lx].temperature;
                    d = std::abs(d) < kMinHeatDiff ? 0.0f : d;
                    const PairConduct& g = pc[row[lx].material * n + below[lx].material];
                    const float da = g.a * d, db = g.b * d;
                    row[lx].temperature -= da;
                    below[lx].temperature += db;
                    rowMax = std::max(rowMax, std::max(std::abs(da), std::abs(db)));
                }
            } else if (down) {
                for (int lx = 0; lx < kChunkSize; ++lx) exchangeOut(row[lx], down->cells[lx], wakeDown);
            }
            maxChange = std::max(maxChange, rowMax);
        }

        // 2. state changes, fire and the ambient
        for (int idx = 0; idx < kChunkCells; ++idx) {
            Cell& c = self_.cells[idx];
            const ThermalProps& p = tp[c.material];
            if (p.events) cellEvents(ox_ + (idx & kChunkMask), oy_ + (idx >> kChunkShift), c, p, keep);
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
    bool queued_ = false; // fallGrain: blocked by a moving grain without moving (keeps its speed)
    int advanceDepth_ = 0; // advance() recursion
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
    anyMiscible_ = false;
    for (std::uint8_t v : mats_.miscibleTable()) anyMiscible_ = anyMiscible_ || v != 0;

    reactPairs_.assign(n * n, 0);
    if (reactions_) {
        const auto& all = reactions_->reactions();
        for (std::size_t i = 0; i < all.size(); ++i) {
            const Reaction& r = all[i];
            const auto v = static_cast<std::uint16_t>(i + 1);
            reactPairs_[static_cast<std::size_t>(r.a) * n + r.b] = v;
            reactPairs_[static_cast<std::size_t>(r.b) * n + r.a] = v;
        }
    }
}

void Simulator::updateChunk(Chunk& chunk, int pass) {
    const std::uint64_t s = Rng::hash(Rng::hash(seed_, tick_ * 4 + static_cast<std::uint64_t>(pass)),
                                      chunkKey(chunk.coord.x, chunk.coord.y));
    const auto stamp = static_cast<std::uint16_t>(tick_ % 65535u + 1u); // never 0 (fresh cells)
    const TickContext ctx{&mats_, &params_, thermal_.data(), pairs_.data(), mats_.size(),
                          params_.reactions ? reactions_ : nullptr, reactPairs_.data(), gas_ != nullptr,
                          mats_.miscibleTable().data(), anyMiscible_, particles_ != nullptr};
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
    double ambient = 0.0, splashIn = 0.0, splashOut = 0.0;
    int splashed = 0;
    emissions_.clear();
    for (Chunk* c : world_.chunks()) {
        processed += c->cellsProcessed;
        events += c->events;
        ambient += c->ambientEnergy;
        splashIn += c->splashIn;
        splashOut += c->splashOut;
        if (!c->gasOut.empty()) {
            emissions_.insert(emissions_.end(), c->gasOut.begin(), c->gasOut.end());
            c->gasOut.clear();
        }
        if (!c->particlesOut.empty()) {
            if (particles_) particles_->add(c->particlesOut.data(), c->particlesOut.size());
            splashed += static_cast<int>(c->particlesOut.size());
            c->particlesOut.clear();
        }
    }
    stats_.splashed = splashed;
    stats_.splashMomentumIn = splashIn;
    stats_.splashMomentumOut = splashOut;
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
