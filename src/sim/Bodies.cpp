#include "Bodies.h"

#include "GasGrid.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <utility>

namespace notita {

namespace {

constexpr float kTickSeconds = 1.0f / 60.0f;
constexpr std::int32_t kHidden = INT32_MIN;

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool terrainKind(MaterialKind k) { return k == MaterialKind::Solid || k == MaterialKind::Powder; }

} // namespace

struct BodySystem::Impl {
    b2WorldId world = b2_nullWorldId;
    std::vector<b2BodyId> ids;          // parallel to bodies_
    b2BodyId terrain = b2_nullBodyId;   // one static body, rebuilt every step
    float ppm = 32.0f;

    float toM(float px) const { return px / ppm; }
    float toPx(float m) const { return m * ppm; }
    float velToM(float pxPerTick) const { return pxPerTick / ppm / kTickSeconds; }
    float velToPx(float mps) const { return mps * ppm * kTickSeconds; }
};

BodySystem::BodySystem(World& world, const MaterialRegistry& mats, ParticleSystem* particles, std::uint64_t seed)
    : world_(world), mats_(mats), particles_(particles), seed_(seed), impl_(std::make_unique<Impl>()) {
    impl_->ppm = params_.pxPerMeter;
    b2WorldDef wd = b2DefaultWorldDef();
    wd.gravity = {0.0f, impl_->velToM(48.0f / 256.0f) / kTickSeconds}; // SimParams::gravity, +y down
    // The terrain shapes are rebuilt every tick, which would wake Box2D's
    // sleepers each time, so rest is judged here (restSpeed) instead.
    wd.enableSleep = false;
    impl_->world = b2CreateWorld(&wd);
}

BodySystem::~BodySystem() {
    if (B2_IS_NON_NULL(impl_->world)) b2DestroyWorld(impl_->world);
}

Rng BodySystem::rng(std::uint64_t salt) const {
    return Rng(Rng::hash(Rng::hash(seed_ ^ 0x9E3779B97F4A7C15ull, tick_), salt));
}

long long BodySystem::totalMass() const {
    long long s = 0;
    for (const RigidBody& b : bodies_)
        for (const BodyPixel& p : b.pixels) s += mats_[p.cell.material].mass;
    return s;
}

double BodySystem::thermalEnergy() const {
    double s = 0;
    for (const RigidBody& b : bodies_)
        for (const BodyPixel& p : b.pixels) s += static_cast<double>(mats_[p.cell.material].heatCapacity) * p.cell.temperature;
    return s;
}

// --------------------------------------------------------------- geometry

void BodySystem::coverBoxes(const std::uint8_t* mask, int w, int h, std::vector<Box>& out) {
    out.clear();
    std::vector<int> open;      // indices into out of boxes whose bottom row is the previous row
    std::vector<int> next;
    for (int y = 0; y < h; ++y) {
        next.clear();
        for (int x = 0; x < w;) {
            if (!mask[y * w + x]) { ++x; continue; }
            int x1 = x;
            while (x1 + 1 < w && mask[y * w + x1 + 1]) ++x1;
            int merged = -1;
            for (int i : open)
                if (out[static_cast<std::size_t>(i)].x0 == x && out[static_cast<std::size_t>(i)].x1 == x1 &&
                    out[static_cast<std::size_t>(i)].y1 == y - 1) { merged = i; break; }
            if (merged >= 0) {
                out[static_cast<std::size_t>(merged)].y1 = y;
                next.push_back(merged);
            } else {
                out.push_back({x, y, x1, y});
                next.push_back(static_cast<int>(out.size()) - 1);
            }
            x = x1 + 1;
        }
        open.swap(next);
    }
}

// --------------------------------------------------------------- clumps

bool BodySystem::flood(int sx, int sy, Clump& out, bool ignorePins) {
    const int X = world_.minX(), Y = world_.minY(), W = world_.widthPx();
    auto idx = [&](int x, int y) { return static_cast<std::size_t>(y - Y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x - X); };
    const Cell* start = world_.cell(sx, sy);
    if (!start) return false;
    const Material& sm = mats_[start->material];
    if (sm.kind != MaterialKind::Solid || (start->flags & kFlagBody) || sm.anchor) return false;
    if (!ignorePins && start->aux == kAuxPinned) return false;
    out.x.clear();
    out.y.clear();
    out.anchored = false;
    out.settled = true;
    out.resting = false;
    stack_.clear();
    stack_.emplace_back(sx, sy);
    visited_[idx(sx, sy)] = 1;
    static constexpr int kDx[4] = {1, -1, 0, 0}, kDy[4] = {0, 0, 1, -1};
    // neighbour lookup through the chunk of the last pixel (most steps stay inside it)
    Chunk* ch = world_.chunkAtPixel(sx, sy);
    auto cellAt = [&](int x, int y) -> const Cell* {
        if (!ch || (x >> kChunkShift) != ch->coord.x || (y >> kChunkShift) != ch->coord.y) ch = world_.chunkAtPixel(x, y);
        return ch ? &ch->at(x & kChunkMask, y & kChunkMask) : nullptr;
    };
    while (!stack_.empty()) {
        const auto [x, y] = stack_.back();
        stack_.pop_back();
        const Cell* c = cellAt(x, y);
        out.x.push_back(x);
        out.y.push_back(y);
        if (c->aux != kAuxSettled && (ignorePins || c->aux != kAuxPinned)) out.settled = false;
        for (int d = 0; d < 4; ++d) {
            const int nx = x + kDx[d], ny = y + kDy[d];
            const Cell* n = cellAt(nx, ny);
            if (!n) { out.anchored = true; continue; } // the world edge is a wall
            const Material& nm = mats_[n->material];
            if (nm.kind != MaterialKind::Solid) {
                if (d == 2 && nm.kind == MaterialKind::Powder) out.resting = true;
                continue;
            }
            if (nm.anchor) { out.anchored = true; continue; }
            // a body's stamp, a soft solid and pinned scene geometry are not part of the
            // clump and don't hold it; they carry it when they are underneath
            const bool pinned = !ignorePins && n->aux == kAuxPinned;
            if ((n->flags & kFlagBody) || !nm.rigid || pinned) {
                if (d == 2) out.resting = true;
                continue;
            }
            if (visited_[idx(nx, ny)]) continue;
            visited_[idx(nx, ny)] = 1;
            stack_.emplace_back(nx, ny);
        }
    }
    return true;
}

void BodySystem::findClumps() {
    const int W = world_.widthPx(), H = world_.heightPx();
    if (W <= 0 || H <= 0) return;
    visited_.assign(static_cast<std::size_t>(W) * static_cast<std::size_t>(H), 0);
    const int X = world_.minX(), Y = world_.minY();
    Clump clump;
    const auto& chunks = world_.chunks();
    for (std::size_t ci = 0; ci < chunks.size(); ++ci) {
        if (!pending_[ci]) continue;
        pending_[ci] = 0;
        Chunk* ch = chunks[ci];
        if (static_cast<int>(bodies_.size()) >= params_.maxBodies) break;
        const int ox = ch->originX(), oy = ch->originY();
        for (int ly = 0; ly < kChunkSize; ++ly)
            for (int lx = 0; lx < kChunkSize; ++lx) {
                const Cell& c = ch->at(lx, ly);
                if (c.material == kEmpty || (c.flags & kFlagBody) || c.aux == kAuxPinned) continue;
                const Material& m = mats_[c.material];
                if (m.kind != MaterialKind::Solid || m.anchor || !m.rigid) continue;
                const int x = ox + lx, y = oy + ly;
                if (visited_[static_cast<std::size_t>(y - Y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x - X)]) continue;
                if (!flood(x, y, clump, false)) continue;
                if (clump.anchored) continue;
                if (clump.settled && clump.resting) continue;
                const int n = static_cast<int>(clump.x.size());
                if (n < params_.minPixels || n > params_.maxPixels) continue;
                if (static_cast<int>(bodies_.size()) >= params_.maxBodies) break;
                createBody(clump, 0.0f, 0.0f, 0.0f);
            }
    }
}

int BodySystem::releaseClump(int x, int y) {
    const int W = world_.widthPx(), H = world_.heightPx();
    if (W <= 0 || H <= 0 || static_cast<int>(bodies_.size()) >= params_.maxBodies) return 0;
    const Cell* c = world_.cell(x, y);
    if (!c || (c->flags & kFlagBody)) return 0;
    visited_.assign(static_cast<std::size_t>(W) * static_cast<std::size_t>(H), 0);
    Clump clump;
    if (!flood(x, y, clump, true)) return 0;
    const int n = static_cast<int>(clump.x.size());
    if (n < 1 || n > params_.maxPixels) return 0;
    return createBody(clump, 0.0f, 0.0f, 0.0f);
}

// --------------------------------------------------------------- bodies

int BodySystem::createBody(const Clump& c, float vx, float vy, float w) {
    const int n = static_cast<int>(c.x.size());
    long long sx = 0, sy = 0;
    for (int i = 0; i < n; ++i) { sx += c.x[static_cast<std::size_t>(i)]; sy += c.y[static_cast<std::size_t>(i)]; }
    const int ox = static_cast<int>(sx / n), oy = static_cast<int>(sy / n);
    std::vector<BodyPixel> pixels;
    pixels.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int x = c.x[static_cast<std::size_t>(i)], y = c.y[static_cast<std::size_t>(i)];
        Cell* cell = world_.cell(x, y);
        BodyPixel p;
        p.lx = static_cast<std::int16_t>(x - ox);
        p.ly = static_cast<std::int16_t>(y - oy);
        p.cell = *cell;
        p.cell.flags = 0;
        p.cell.stamp = 0;
        p.cell.aux = 0;
        p.cell.vx = p.cell.vy = 0;
        pixels.push_back(p);
        *cell = Cell{};
        world_.wake(x, y);
    }
    return createBody(std::move(pixels), static_cast<float>(ox), static_cast<float>(oy), 1.0f, 0.0f, vx, vy, w, 0);
}

int BodySystem::createBody(std::vector<BodyPixel>&& pixels, float x, float y, float cs, float sn, float vx, float vy, float w,
                           int cooldown) {
    RigidBody b;
    b.id = nextId_++;
    b.pixels = std::move(pixels);
    b.x = x;
    b.y = y;
    b.c = cs;
    b.s = sn;
    b.vx = vx;
    b.vy = vy;
    b.w = w;
    b.cooldown = cooldown;
    b.stampX.assign(b.pixels.size(), kHidden);
    b.stampY.assign(b.pixels.size(), kHidden);
    for (const BodyPixel& p : b.pixels) b.mass += mats_[p.cell.material].mass;

    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_dynamicBody;
    bd.position = b2ToPos({impl_->toM(x), impl_->toM(y)});
    bd.rotation = {cs, sn};
    bd.linearVelocity = {impl_->velToM(vx), impl_->velToM(vy)};
    bd.angularVelocity = w / kTickSeconds;
    bd.linearDamping = 0.02f;
    bd.angularDamping = 0.05f;
    bd.isAwake = true;
    b2BodyId id = b2CreateBody(impl_->world, &bd);
    bodies_.push_back(std::move(b));
    impl_->ids.push_back(id);
    buildShapes(bodies_.back());
    ++stats_.created;
    return bodies_.back().id;
}

void BodySystem::buildShapes(RigidBody& b) {
    const std::size_t i = static_cast<std::size_t>(&b - bodies_.data());
    b2BodyId id = impl_->ids[i];
    // drop the old shapes
    const int count = b2Body_GetShapeCount(id);
    if (count > 0) {
        std::vector<b2ShapeId> shapes(static_cast<std::size_t>(count));
        b2Body_GetShapes(id, shapes.data(), count);
        for (b2ShapeId s : shapes) b2DestroyShape(s, false);
    }
    if (b.pixels.empty()) { b.rebuild = false; return; }
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (const BodyPixel& p : b.pixels) {
        x0 = std::min<int>(x0, p.lx); x1 = std::max<int>(x1, p.lx);
        y0 = std::min<int>(y0, p.ly); y1 = std::max<int>(y1, p.ly);
    }
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(w) * h, 0);
    std::vector<MaterialId> mat(static_cast<std::size_t>(w) * h, kEmpty);
    for (const BodyPixel& p : b.pixels) {
        const std::size_t k = static_cast<std::size_t>(p.ly - y0) * w + static_cast<std::size_t>(p.lx - x0);
        mask[k] = 1;
        mat[k] = p.cell.material;
    }
    std::vector<Box> boxes;
    coverBoxes(mask.data(), w, h, boxes);
    for (const Box& bx : boxes) {
        // density: the mean of the pixels in the box (kg/m^3 / 1000 -> Box2D's kg/m^2-ish)
        float dens = 0.0f, fric = 0.0f, rest = 0.0f;
        int n = 0;
        for (int yy = bx.y0; yy <= bx.y1; ++yy)
            for (int xx = bx.x0; xx <= bx.x1; ++xx) {
                const Material& m = mats_[mat[static_cast<std::size_t>(yy) * w + xx]];
                dens += m.density;
                fric += m.friction;
                rest += m.bounce;
                ++n;
            }
        b2ShapeDef sd = b2DefaultShapeDef();
        sd.density = std::max(0.05f, dens / static_cast<float>(n) / 1000.0f);
        sd.material.friction = fric / static_cast<float>(n);
        sd.material.restitution = rest / static_cast<float>(n);
        const float hw = 0.5f * static_cast<float>(bx.x1 - bx.x0 + 1), hh = 0.5f * static_cast<float>(bx.y1 - bx.y0 + 1);
        const float cx = static_cast<float>(x0 + bx.x0) + hw, cy = static_cast<float>(y0 + bx.y0) + hh;
        const b2Polygon poly = b2MakeOffsetBox(impl_->toM(hw), impl_->toM(hh), {impl_->toM(cx), impl_->toM(cy)}, b2Rot_identity);
        b2CreatePolygonShape(id, &sd, &poly);
    }
    b2Body_UpdateMassFromShapes(id);
    b.rebuild = false;
}

void BodySystem::destroyBody(RigidBody& b) {
    const std::size_t i = static_cast<std::size_t>(&b - bodies_.data());
    b2DestroyBody(impl_->ids[i]);
    impl_->ids.erase(impl_->ids.begin() + static_cast<std::ptrdiff_t>(i));
    bodies_.erase(bodies_.begin() + static_cast<std::ptrdiff_t>(i));
}

void BodySystem::pointVelocity(const RigidBody& b, float wx, float wy, float& vx, float& vy) const {
    // v + w x r, r from the body origin (close enough to the centre of mass for pixels)
    const float rx = wx - b.x, ry = wy - b.y;
    vx = b.vx - b.w * ry;
    vy = b.vy + b.w * rx;
}

void BodySystem::eject(const Cell& cell, float x, float y, float vx, float vy) {
    Particle p;
    p.x = x;
    p.y = y;
    p.vx = vx;
    p.vy = vy;
    p.cell = cell;
    p.cell.stamp = 0;
    p.cell.flags = 0;
    p.cell.aux = 0;
    p.cell.vx = p.cell.vy = 0;
    particles_->add(p);
}

// Take the body's pixels out of the grid. A stamped cell that is no longer
// this pixel (melted, burnt, reacted, erased) has left the body: it stays in
// the world as whatever it became and the body loses that pixel.
void BodySystem::unstamp(RigidBody& b) {
    std::size_t keep = 0;
    for (std::size_t i = 0; i < b.pixels.size(); ++i) {
        BodyPixel& p = b.pixels[i];
        const std::int32_t sx = b.stampX[i], sy = b.stampY[i];
        bool lost = false;
        if (sx != kHidden) {
            Cell* c = world_.cell(sx, sy);
            if (c && (c->flags & kFlagBody) && c->material == p.cell.material) {
                p.cell.temperature = c->temperature;
                p.cell.life = c->life;
                p.cell.flags = c->flags & kFlagBurning;
                *c = Cell{};
                world_.wake(sx, sy);
            } else {
                lost = true;
            }
        }
        if (lost) {
            b.mass -= mats_[p.cell.material].mass;
            b.rebuild = true;
            continue;
        }
        if (keep != i) b.pixels[keep] = p;
        ++keep;
    }
    b.pixels.resize(keep);
    b.stampX.assign(keep, kHidden);
    b.stampY.assign(keep, kHidden);
}

// Write the body's pixels into the grid at its current transform. Loose
// pixels in the way are shoved out as particles with the body's velocity;
// solids and other bodies' pixels hide this pixel for the tick.
void BodySystem::stamp(RigidBody& b) {
    const float airCap = mats_[kEmpty].heatCapacity;
    b.hidden = 0;
    for (std::size_t i = 0; i < b.pixels.size(); ++i) {
        BodyPixel& p = b.pixels[i];
        float wx, wy;
        b.toWorld(p, wx, wy);
        const int x = static_cast<int>(std::floor(wx)), y = static_cast<int>(std::floor(wy));
        Cell* c = world_.cell(x, y);
        if (!c || (c->flags & kFlagBody) || !mats_[c->material].isFluid()) {
            b.stampX[i] = b.stampY[i] = kHidden;
            ++b.hidden;
            continue;
        }
        if (c->material != kEmpty) {
            float vx, vy;
            pointVelocity(b, wx, wy, vx, vy);
            eject(*c, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, vx, vy);
            ++stats_.displaced;
        } else {
            const Material& m = mats_[p.cell.material];
            p.cell.temperature = std::clamp(p.cell.temperature + airCap * (c->temperature - kAmbientTemp) / m.heatCapacity, kMinTemp, kMaxTemp);
        }
        Cell cell = p.cell;
        cell.flags = static_cast<std::uint8_t>(kFlagBody | (p.cell.flags & kFlagBurning));
        cell.stamp = 0;
        cell.aux = 0;
        *c = cell;
        world_.wake(x, y);
        b.stampX[i] = x;
        b.stampY[i] = y;
    }
}

// A body that has slept long enough becomes plain settled pixels. Every
// pixel must land: the nearest free cell within a few pixels, else it flies
// off as a particle.
void BodySystem::freeze(RigidBody& b) {
    for (const BodyPixel& p : b.pixels) {
        float wx, wy;
        b.toWorld(p, wx, wy);
        const int x = static_cast<int>(std::floor(wx)), y = static_cast<int>(std::floor(wy));
        Cell* dst = nullptr;
        int dx = 0, dy = 0;
        for (int r = 0; r <= 3 && !dst; ++r)
            for (int yy = -r; yy <= r && !dst; ++yy)
                for (int xx = -r; xx <= r && !dst; ++xx) {
                    if (std::max(std::abs(xx), std::abs(yy)) != r) continue;
                    Cell* c = world_.cell(x + xx, y + yy);
                    if (c && c->material == kEmpty) { dst = c; dx = xx; dy = yy; }
                }
        if (!dst) {
            eject(p.cell, wx, wy, b.vx, b.vy);
            continue;
        }
        Cell cell = p.cell;
        cell.flags = 0;
        cell.stamp = 0;
        cell.aux = kAuxSettled;
        *dst = cell;
        world_.wake(x + dx, y + dy);
    }
    ++stats_.frozen;
    destroyBody(b);
}

// Hard impact: split the pixels by nearest of a few random seeds, then into
// connected pieces. Big pieces become bodies with the parent's velocity at
// their centre; crumbs fly off as particles.
void BodySystem::shatter(RigidBody& b) {
    Rng r = rng(0x5A5A + static_cast<std::uint64_t>(b.id));
    const int n = static_cast<int>(b.pixels.size());
    const int seeds = std::clamp(2 + n / 40, 2, 6);
    std::vector<int> seedIdx(static_cast<std::size_t>(seeds));
    for (int& s : seedIdx) s = static_cast<int>(r.below(static_cast<std::uint32_t>(n)));
    std::vector<int> owner(static_cast<std::size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
        int best = 0;
        long long bestD = LLONG_MAX;
        for (int s = 0; s < seeds; ++s) {
            const BodyPixel& a = b.pixels[static_cast<std::size_t>(i)];
            const BodyPixel& q = b.pixels[static_cast<std::size_t>(seedIdx[static_cast<std::size_t>(s)])];
            const long long d = static_cast<long long>(a.lx - q.lx) * (a.lx - q.lx) + static_cast<long long>(a.ly - q.ly) * (a.ly - q.ly);
            if (d < bestD) { bestD = d; best = s; }
        }
        owner[static_cast<std::size_t>(i)] = best;
    }
    // connected components within each seed group (4-connected, via a local grid)
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (const BodyPixel& p : b.pixels) {
        x0 = std::min<int>(x0, p.lx); x1 = std::max<int>(x1, p.lx);
        y0 = std::min<int>(y0, p.ly); y1 = std::max<int>(y1, p.ly);
    }
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    std::vector<int> grid(static_cast<std::size_t>(w) * h, -1);
    for (int i = 0; i < n; ++i) {
        const BodyPixel& p = b.pixels[static_cast<std::size_t>(i)];
        grid[static_cast<std::size_t>(p.ly - y0) * w + (p.lx - x0)] = i;
    }
    std::vector<int> comp(static_cast<std::size_t>(n), -1);
    std::vector<std::vector<int>> pieces;
    std::vector<int> stack;
    for (int i = 0; i < n; ++i) {
        if (comp[static_cast<std::size_t>(i)] >= 0) continue;
        const int id = static_cast<int>(pieces.size());
        pieces.emplace_back();
        comp[static_cast<std::size_t>(i)] = id;
        stack.assign(1, i);
        while (!stack.empty()) {
            const int k = stack.back();
            stack.pop_back();
            pieces.back().push_back(k);
            const BodyPixel& p = b.pixels[static_cast<std::size_t>(k)];
            static constexpr int kDx[4] = {1, -1, 0, 0}, kDy[4] = {0, 0, 1, -1};
            for (int d = 0; d < 4; ++d) {
                const int gx = p.lx - x0 + kDx[d], gy = p.ly - y0 + kDy[d];
                if (gx < 0 || gy < 0 || gx >= w || gy >= h) continue;
                const int j = grid[static_cast<std::size_t>(gy) * w + gx];
                if (j < 0 || comp[static_cast<std::size_t>(j)] >= 0 || owner[static_cast<std::size_t>(j)] != owner[static_cast<std::size_t>(k)]) continue;
                comp[static_cast<std::size_t>(j)] = id;
                stack.push_back(j);
            }
        }
    }
    // the parent's state, then the parent goes
    const RigidBody parent = b;
    destroyBody(b);
    ++stats_.shattered;
    for (const std::vector<int>& piece : pieces) {
        if (static_cast<int>(piece.size()) < params_.minPixels) {
            for (int k : piece) {
                const BodyPixel& p = parent.pixels[static_cast<std::size_t>(k)];
                float wx, wy, vx, vy;
                parent.toWorld(p, wx, wy);
                pointVelocity(parent, wx, wy, vx, vy);
                eject(p.cell, wx, wy, vx + (r.unit() - 0.5f) * 2.0f, vy - r.unit() * 1.5f);
            }
            continue;
        }
        if (static_cast<int>(bodies_.size()) >= params_.maxBodies) {
            // no room for another body: the piece drops as pixels where it is
            for (int k : piece) {
                const BodyPixel& p = parent.pixels[static_cast<std::size_t>(k)];
                float wx, wy;
                parent.toWorld(p, wx, wy);
                eject(p.cell, wx, wy, parent.vx, parent.vy);
            }
            continue;
        }
        // the piece keeps the parent's transform (same origin and rotation), so its pixels don't jump
        std::vector<BodyPixel> px;
        px.reserve(piece.size());
        double cx = 0, cy = 0;
        for (int k : piece) {
            px.push_back(parent.pixels[static_cast<std::size_t>(k)]);
            float wx, wy;
            parent.toWorld(px.back(), wx, wy);
            cx += wx;
            cy += wy;
        }
        cx /= static_cast<double>(piece.size());
        cy /= static_cast<double>(piece.size());
        float vx, vy;
        pointVelocity(parent, static_cast<float>(cx), static_cast<float>(cy), vx, vy);
        vx += (r.unit() - 0.5f) * 1.0f;
        vy -= r.unit() * 0.5f;
        createBody(std::move(px), parent.x, parent.y, parent.c, parent.s, vx, vy, parent.w, params_.shatterCooldown);
    }
}

// --------------------------------------------------------------- the step

void BodySystem::buildTerrain() {
    stats_.terrainShapes = 0;
    if (B2_IS_NON_NULL(impl_->terrain)) {
        b2DestroyBody(impl_->terrain);
        impl_->terrain = b2_nullBodyId;
    }
    if (bodies_.empty()) return;
    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_staticBody;
    impl_->terrain = b2CreateBody(impl_->world, &bd);
    b2ShapeDef sd = b2DefaultShapeDef();
    sd.material.friction = 0.7f;
    std::vector<std::uint8_t> mask;
    std::vector<Box> boxes;
    for (const RigidBody& b : bodies_) {
        // the body's pixel box at its transform, grown by what it can move this tick
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const BodyPixel& p : b.pixels) {
            float wx, wy;
            b.toWorld(p, wx, wy);
            x0 = std::min(x0, wx); x1 = std::max(x1, wx);
            y0 = std::min(y0, wy); y1 = std::max(y1, wy);
        }
        const float reach = std::sqrt(b.vx * b.vx + b.vy * b.vy) + 3.0f + std::abs(b.w) * std::max(x1 - x0, y1 - y0);
        const int ax0 = static_cast<int>(std::floor(x0 - reach)), ay0 = static_cast<int>(std::floor(y0 - reach));
        const int ax1 = static_cast<int>(std::ceil(x1 + reach)), ay1 = static_cast<int>(std::ceil(y1 + reach));
        const int w = ax1 - ax0 + 1, h = ay1 - ay0 + 1;
        if (w <= 0 || h <= 0 || static_cast<long long>(w) * h > 1 << 20) continue;
        mask.assign(static_cast<std::size_t>(w) * h, 0);
        for (int y = ay0; y <= ay1; ++y)
            for (int x = ax0; x <= ax1; ++x) {
                const Cell* c = world_.cell(x, y);
                const bool wall = !c || (terrainKind(mats_[c->material].kind) && !(c->flags & kFlagBody));
                mask[static_cast<std::size_t>(y - ay0) * w + (x - ax0)] = wall;
            }
        coverBoxes(mask.data(), w, h, boxes);
        for (const Box& bx : boxes) {
            const float hw = 0.5f * static_cast<float>(bx.x1 - bx.x0 + 1), hh = 0.5f * static_cast<float>(bx.y1 - bx.y0 + 1);
            const float cx = static_cast<float>(ax0 + bx.x0) + hw, cy = static_cast<float>(ay0 + bx.y0) + hh;
            const b2Polygon poly = b2MakeOffsetBox(impl_->toM(hw), impl_->toM(hh), {impl_->toM(cx), impl_->toM(cy)}, b2Rot_identity);
            b2CreatePolygonShape(impl_->terrain, &sd, &poly);
            ++stats_.terrainShapes;
        }
    }
}

void BodySystem::applyGas(RigidBody& b) {
    if (!gas_ || (params_.gasDrag <= 0.0f && params_.gasPush <= 0.0f)) return;
    const GasFields& f = gas_->fields();
    if (f.w <= 0) return;
    const std::size_t i = static_cast<std::size_t>(&b - bodies_.data());
    const b2BodyId id = impl_->ids[i];
    const b2Vec2 com = b2ToVec2(b2Body_GetWorldCenter(id));
    const int gi = (static_cast<int>(std::floor(impl_->toPx(com.x))) - gas_->originX()) / kGasCell;
    const int gj = (static_cast<int>(std::floor(impl_->toPx(com.y))) - gas_->originY()) / kGasCell;
    if (gi < 0 || gj < 0 || gi >= f.w || gj >= f.h) return;
    const int c = gj * f.w + gi;
    // gradient of the pressure around the body (per pixel), open cells only
    auto pAt = [&](int ii, int jj) {
        if (ii < 0 || jj < 0 || ii >= f.w || jj >= f.h) return f.p[c];
        const int k = jj * f.w + ii;
        return f.open[k] > 0.0f ? f.p[k] : f.p[c];
    };
    const float gpx = (pAt(gi + 1, gj) - pAt(gi - 1, gj)) / (2.0f * kGasCell);
    const float gpy = (pAt(gi, gj + 1) - pAt(gi, gj - 1)) / (2.0f * kGasCell);
    float ax = -params_.gasPush * gpx, ay = -params_.gasPush * gpy; // px/tick^2
    if (f.open[c] > 0.0f) {
        const float gx = f.uc(gi, gj) * kGasCell, gy = f.vc(gi, gj) * kGasCell;
        ax += params_.gasDrag * (gx - b.vx);
        ay += params_.gasDrag * (gy - b.vy);
    }
    if (ax == 0.0f && ay == 0.0f) return;
    const float mass = b2Body_GetMass(id);
    const float k = impl_->velToM(1.0f) / kTickSeconds; // px/tick^2 -> m/s^2
    b2Body_ApplyForceToCenter(id, {mass * ax * k, mass * ay * k}, true);
}

void BodySystem::explode(int x, int y, int radius, float power) {
    if (radius <= 0 || power <= 0.0f) return;
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
        RigidBody& b = bodies_[i];
        float cx = 0, cy = 0;
        for (const BodyPixel& p : b.pixels) {
            float wx, wy;
            b.toWorld(p, wx, wy);
            cx += wx;
            cy += wy;
        }
        if (b.pixels.empty()) continue;
        cx /= static_cast<float>(b.pixels.size());
        cy /= static_cast<float>(b.pixels.size());
        const float dx = cx - static_cast<float>(x), dy = cy - static_cast<float>(y);
        const float d = std::sqrt(dx * dx + dy * dy);
        const float r = static_cast<float>(radius) * 2.0f; // blasts reach bodies a bit further than pixels
        if (d > r) continue;
        const float fall = 1.0f - d / (r + 1.0f);
        float nx = d > 0.5f ? dx / d : 0.0f, ny = (d > 0.5f ? dy / d : 0.0f) - 0.6f;
        const float len = std::sqrt(nx * nx + ny * ny);
        nx /= len;
        ny /= len;
        const float dv = params_.blastKick * power * fall; // px/tick
        const b2BodyId id = impl_->ids[i];
        const float mass = b2Body_GetMass(id);
        b2Body_ApplyLinearImpulseToCenter(id, {mass * impl_->velToM(dv * nx), mass * impl_->velToM(dv * ny)}, true);
        b2Body_ApplyAngularImpulse(id, b2Body_GetRotationalInertia(id) * (rng(static_cast<std::uint64_t>(b.id)).unit() - 0.5f) * dv * 0.2f, true);
        b.awake = true;
    }
}

void BodySystem::step() {
    const auto t0 = std::chrono::steady_clock::now();
    ++tick_;
    stats_.created = stats_.frozen = stats_.shattered = stats_.displaced = 0;
    if (!params_.enabled) {
        stats_.stepMs = msSince(t0);
        return;
    }
    if (impl_->ppm != params_.pxPerMeter) impl_->ppm = params_.pxPerMeter; // only affects new bodies' shapes

    // 1. the bodies leave the grid (pixels that changed while stamped leave the body)
    for (RigidBody& b : bodies_) unstamp(b);

    // 2. new bodies from clumps that lost their support
    const auto t1 = std::chrono::steady_clock::now();
    {
        const auto& chunks = world_.chunks();
        if (pending_.size() != chunks.size()) pending_.assign(chunks.size(), 1);
        for (std::size_t ci = 0; ci < chunks.size(); ++ci)
            if (chunks[ci]->active()) pending_[ci] = 1;
    }
    if (tick_ % static_cast<std::uint64_t>(std::max(1, params_.checkInterval)) == 0) findClumps();
    stats_.findMs = msSince(t1);

    // 3. shapes for bodies that lost pixels; bodies with too few pixels crumble
    for (std::size_t i = 0; i < bodies_.size();) {
        RigidBody& b = bodies_[i];
        if (static_cast<int>(b.pixels.size()) < std::max(1, params_.minPixels / 2)) {
            for (const BodyPixel& p : b.pixels) {
                float wx, wy;
                b.toWorld(p, wx, wy);
                eject(p.cell, wx, wy, b.vx, b.vy);
            }
            destroyBody(b);
            continue;
        }
        if (b.rebuild) buildShapes(b);
        ++i;
    }

    // 4. one Box2D step against the pixel terrain, with the gas grid pushing
    const auto t2 = std::chrono::steady_clock::now();
    buildTerrain();
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
        RigidBody& b = bodies_[i];
        applyGas(b);
        const b2Vec2 v = b2Body_GetLinearVelocity(impl_->ids[i]); // the velocity going into the step
        b.vx = impl_->velToPx(v.x);
        b.vy = impl_->velToPx(v.y);
    }
    if (!bodies_.empty()) b2World_Step(impl_->world, kTickSeconds, std::max(1, params_.subSteps));
    stats_.solveMs = msSince(t2);

    // 5. read back, shatter on hard impacts, freeze sleepers
    const std::size_t stepped = bodies_.size(); // fragments appended below are left alone this tick
    for (std::size_t i = 0; i < bodies_.size();) {
        RigidBody& b = bodies_[i];
        const b2BodyId id = impl_->ids[i];
        const b2WorldTransform t = b2Body_GetTransform(id);
        const b2Vec2 p = b2ToVec2(t.p);
        b.x = impl_->toPx(p.x);
        b.y = impl_->toPx(p.y);
        b.c = t.q.c;
        b.s = t.q.s;
        const float v0x = b.vx, v0y = b.vy;
        const b2Vec2 v = b2Body_GetLinearVelocity(id);
        b.vx = impl_->velToPx(v.x);
        b.vy = impl_->velToPx(v.y);
        b.w = b2Body_GetAngularVelocity(id) * kTickSeconds;
        b.awake = std::sqrt(b.vx * b.vx + b.vy * b.vy) > params_.restSpeed || std::abs(b.w) > params_.restSpeed * 0.05f;
        if (i >= stepped) { ++i; continue; }
        if (b.cooldown > 0) --b.cooldown;
        const float dvx = b.vx - v0x, dvy = b.vy - v0y;
        const float dv = std::sqrt(dvx * dvx + dvy * dvy);
        if (dv >= params_.shatterSpeed && b.cooldown == 0 && static_cast<int>(b.pixels.size()) >= 2 * params_.minPixels) {
            shatter(b); // replaces this entry; fragments are appended
            continue;
        }
        b.asleep = b.awake ? 0 : b.asleep + 1;
        if (b.asleep >= params_.freezeAfter) {
            freeze(b);
            continue;
        }
        ++i;
    }

    // 6. back into the grid
    int hidden = 0, awake = 0, pixels = 0;
    for (RigidBody& b : bodies_) {
        stamp(b);
        hidden += b.hidden;
        awake += b.awake;
        pixels += static_cast<int>(b.pixels.size());
    }
    stats_.bodies = static_cast<int>(bodies_.size());
    stats_.awake = awake;
    stats_.pixels = pixels;
    stats_.hidden = hidden;
    stats_.stepMs = msSince(t0);
}

void BodySystem::clear() {
    for (RigidBody& b : bodies_) unstamp(b);
    while (!bodies_.empty()) freeze(bodies_.back());
    if (B2_IS_NON_NULL(impl_->terrain)) {
        b2DestroyBody(impl_->terrain);
        impl_->terrain = b2_nullBodyId;
    }
}

} // namespace notita
