#include "Simulator.h"

#include "Rng.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace notita {

namespace {

// Everything one chunk update needs. Lives on the worker's stack.
class ChunkUpdate {
public:
    ChunkUpdate(World& world, const MaterialRegistry& mats, Chunk& self, std::uint64_t seed,
                std::uint16_t stamp, const SimParams& params)
        : mats_(mats), self_(self), rng_(seed), stamp_(stamp), params_(params) {
        ox_ = self.originX();
        oy_ = self.originY();
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                nb_[dy + 1][dx + 1] = world.chunk(self.coord.x + dx, self.coord.y + dy);
    }

    std::uint32_t run() {
        const DirtyRect r = self_.rect;
        for (int ly = r.y1; ly >= r.y0; --ly) {        // bottom to top: falling stuff moves once
            const bool leftToRight = rng_.coin();       // alternate scan direction: no drift bias
            for (int i = r.x0; i <= r.x1; ++i) {
                const int lx = leftToRight ? i : r.x1 - (i - r.x0);
                Cell& c = self_.at(lx, ly);
                if (c.stamp == stamp_) continue;
                const Material& m = mats_[c.material];
                switch (m.kind) {
                case MaterialKind::Powder: ++processed_; updatePowder(ox_ + lx, oy_ + ly, &c, m); break;
                case MaterialKind::Liquid: ++processed_; updateLiquid(ox_ + lx, oy_ + ly, &c, m); break;
                case MaterialKind::Gas:    ++processed_; updateGas(ox_ + lx, oy_ + ly, &c, m); break;
                default: break;
                }
            }
        }
        if (!selfNext_.empty()) self_.nextRect.include(selfNext_.x0, selfNext_.y0, selfNext_.x1, selfNext_.y1);
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
            Cell* l = at(x - 1, y);
            Cell* r = at(x + 1, y);
            if ((l && canDisplace(m, mats_[l->material], 0)) || (r && canDisplace(m, mats_[r->material], 0))) wake(x, y);
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

    const MaterialRegistry& mats_;
    Chunk& self_;
    Chunk* nb_[3][3]{};
    int ox_ = 0, oy_ = 0;
    Rng rng_;
    std::uint16_t stamp_;
    const SimParams& params_;
    DirtyRect selfNext_;
    std::uint32_t processed_ = 0;
};

static_assert(16 + 1 + 1 < kMaxReach, "max fall / spread must stay inside the checkerboard margin");

} // namespace

Simulator::Simulator(World& world, const MaterialRegistry& materials, ThreadPool* pool, std::uint64_t seed)
    : world_(world), mats_(materials), pool_(pool), seed_(seed) {}

void Simulator::updateChunk(Chunk& chunk, int pass) {
    const std::uint64_t s = Rng::hash(Rng::hash(seed_, tick_ * 4 + static_cast<std::uint64_t>(pass)),
                                      chunkKey(chunk.coord.x, chunk.coord.y));
    const auto stamp = static_cast<std::uint16_t>(tick_ % 65535u + 1u); // never 0 (fresh cells)
    ChunkUpdate u(world_, mats_, chunk, s, stamp, params_);
    chunk.cellsProcessed = u.run();
}

void Simulator::tick() {
    const auto t0 = std::chrono::steady_clock::now();
    ++tick_;
    stats_.activeChunks = world_.beginTick();
    stats_.totalChunks = static_cast<int>(world_.chunks().size());

    for (int pass = 0; pass < 4; ++pass) {
        const int px = pass & 1, py = pass >> 1;
        jobs_.clear();
        for (Chunk* c : world_.chunks())
            if (c->active() && (c->coord.x & 1) == px && (c->coord.y & 1) == py) jobs_.push_back(c);
        if (jobs_.empty()) continue;
        const std::function<void(int)> job = [&](int i) { updateChunk(*jobs_[i], pass); };
        if (pool_) pool_->parallelFor(static_cast<int>(jobs_.size()), job);
        else for (int i = 0; i < static_cast<int>(jobs_.size()); ++i) job(i);
    }

    std::uint64_t processed = 0;
    for (const Chunk* c : world_.chunks()) processed += c->cellsProcessed;
    stats_.cellsProcessed = processed;
    stats_.tick = tick_;
    stats_.tickMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace notita
