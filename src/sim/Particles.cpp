#include "Particles.h"

#include "GasGrid.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace notita {

ParticleSystem::ParticleSystem(World& world, const MaterialRegistry& mats, ThreadPool* pool, std::uint64_t seed)
    : world_(world), mats_(mats), pool_(pool), seed_(seed) {}

bool ParticleSystem::launch(int x, int y, float vx, float vy, float ambient) {
    Cell* c = world_.cell(x, y);
    if (!c || c->material == kEmpty || (c->flags & kFlagBody)) return false;
    if (static_cast<int>(particles_.size()) >= params_.maxCount) return false;
    Particle p;
    p.x = static_cast<float>(x) + 0.5f;
    p.y = static_cast<float>(y) + 0.5f;
    p.vx = vx;
    p.vy = vy;
    p.cell = *c;
    p.cell.stamp = 0;
    p.cell.flags = 0;
    p.cell.vx = p.cell.vy = 0;
    *c = Cell{};
    c->temperature = ambient;
    world_.wake(x, y);
    particles_.push_back(p);
    ++spawned_;
    return true;
}

void ParticleSystem::add(const Particle& p) {
    if (static_cast<int>(particles_.size()) >= params_.maxCount) return;
    particles_.push_back(p);
    ++spawned_;
}

void ParticleSystem::add(const Particle* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) add(p[i]);
}

void ParticleSystem::move(Particle& p, const Rng&) {
    const Material& m = mats_[p.cell.material];
    const bool flat = world_.gravity().flat(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)));
    if (!flat) p.vy += params_.gravity * m.gravity;
    const float drag = flat ? params_.flatDrag : params_.drag;
    p.vx *= 1.0f - drag;
    p.vy *= 1.0f - drag;
    if (gas_ && params_.gasDrag > 0.0f) {
        const GasFields& f = gas_->fields();
        const int i = static_cast<int>(std::floor(p.x)) - gas_->originX(), j = static_cast<int>(std::floor(p.y)) - gas_->originY();
        const int gi = i >> 2, gj = j >> 2;
        if (f.w > 0 && i >= 0 && j >= 0 && gi < f.w && gj < f.h && f.open[gj * f.w + gi] > 0.0f) {
            const float gx = f.uc(gi, gj) * kGasCell, gy = f.vc(gi, gj) * kGasCell;
            p.vx += params_.gasDrag * (gx - p.vx);
            p.vy += params_.gasDrag * (gy - p.vy);
        }
    }
    const float speed = std::sqrt(p.vx * p.vx + p.vy * p.vy);
    if (speed > params_.maxSpeed) {
        p.vx *= params_.maxSpeed / speed;
        p.vy *= params_.maxSpeed / speed;
    }
    if (p.age < 65535) ++p.age;

    const int steps = static_cast<int>(std::ceil(std::max(std::abs(p.vx), std::abs(p.vy))));
    int cx = static_cast<int>(std::floor(p.x)), cy = static_cast<int>(std::floor(p.y));
    bool blocked = false;
    if (steps > 0) {
        const float dx = p.vx / static_cast<float>(steps), dy = p.vy / static_cast<float>(steps);
        const Chunk* ch = world_.chunkAtPixel(cx, cy);
        for (int s = 0; s < steps; ++s) {
            const float nx = p.x + dx, ny = p.y + dy;
            const int tx = static_cast<int>(std::floor(nx)), ty = static_cast<int>(std::floor(ny));
            if (tx != cx || ty != cy) {
                if (!ch || (tx >> kChunkShift) != (cx >> kChunkShift) || (ty >> kChunkShift) != (cy >> kChunkShift))
                    ch = world_.chunkAtPixel(tx, ty);
                if (!ch || ch->at(tx & kChunkMask, ty & kChunkMask).material != kEmpty) {
                    blocked = true;
                    break;
                }
                cx = tx;
                cy = ty;
            }
            p.x = nx;
            p.y = ny;
        }
    }
    if (!blocked && speed < params_.settleSpeed) {
        const Cell* below = world_.cell(cx, cy + 1);
        if (flat || !below || below->material != kEmpty) blocked = true; // #483: seen from above it lands on the floor
    }
    if (p.age > params_.maxAge) blocked = true;
    if (blocked) p.rest = 1; else p.rest = 0;
}

bool ParticleSystem::settle(Particle& p, int cx, int cy) {
    // the cell it is in, else the nearest empty one close by (another particle may have taken it)
    static constexpr int kDx[9] = {0, 0, -1, 1, 0, -1, 1, -1, 1}, kDy[9] = {0, 1, 0, 0, -1, 1, 1, -1, -1};
    Cell* dst = nullptr;
    int sx = cx, sy = cy;
    for (int k = 0; k < 9 && !dst; ++k) {
        Cell* c = world_.cell(cx + kDx[k], cy + kDy[k]);
        if (c && c->material == kEmpty) { dst = c; sx = cx + kDx[k]; sy = cy + kDy[k]; }
    }
    if (!dst) return false;
    const Material& m = mats_[p.cell.material];
    const float airCap = mats_[kEmpty].heatCapacity;
    Cell c = p.cell;
    c.stamp = 0;
    c.flags = 0;
    // the air pixel's heat (relative to ambient) goes into the landing pixel
    c.temperature = std::clamp(c.temperature + airCap * (dst->temperature - kAmbientTemp) / m.heatCapacity, kMinTemp, kMaxTemp);
    const float vmax = static_cast<float>(m.maxFall);
    c.vx = static_cast<std::int16_t>(std::clamp(p.vx, -vmax, vmax) * kVelOne);
    // the automaton only carries falling speed (#483: in a top-down region, speed along the floor)
    c.vy = static_cast<std::int16_t>(std::clamp(p.vy, world_.gravity().flat(sx, sy) ? -vmax : 0.0f, vmax) * kVelOne);
    *dst = c;
    world_.wake(sx, sy);
    return true;
}

void ParticleSystem::extractIn(int x0, int y0, int x1, int y1, std::vector<Particle>& out) {
    std::size_t keep = 0;
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        const Particle& p = particles_[i];
        const int cx = static_cast<int>(std::floor(p.x)), cy = static_cast<int>(std::floor(p.y));
        if (cx >= x0 && cx < x1 && cy >= y0 && cy < y1) { out.push_back(p); continue; }
        if (keep != i) particles_[keep] = p;
        ++keep;
    }
    particles_.resize(keep);
}

void ParticleSystem::step() {
    const auto t0 = std::chrono::steady_clock::now();
    ++tick_;
    const int n = static_cast<int>(particles_.size());
    stats_.spawned = spawned_;
    spawned_ = 0;
    stats_.settled = 0;
    stats_.waiting = 0;
    if (n > 0) {
        const Rng none;
        constexpr int kBlock = 512;
        const int blocks = (n + kBlock - 1) / kBlock;
        auto job = [&](int b) {
            const int end = std::min(n, (b + 1) * kBlock);
            for (int i = b * kBlock; i < end; ++i) move(particles_[static_cast<std::size_t>(i)], none);
        };
        if (pool_ && blocks > 1) pool_->parallelFor(blocks, job);
        else for (int b = 0; b < blocks; ++b) job(b);

        std::size_t keep = 0;
        for (std::size_t i = 0; i < particles_.size(); ++i) {
            Particle& p = particles_[i];
            if (p.rest) {
                const int cx = static_cast<int>(std::floor(p.x)), cy = static_cast<int>(std::floor(p.y));
                if (settle(p, cx, cy)) { ++stats_.settled; continue; }
                // no room: wait, and if something flowed over it meanwhile, climb out one cell per tick
                p.vx = p.vy = 0.0f;
                const Cell* host = world_.cell(cx, cy);
                if (host && host->material != kEmpty && cy > world_.minY()) p.y -= 1.0f;
                ++stats_.waiting;
            }
            if (keep != i) particles_[keep] = p;
            ++keep;
        }
        particles_.resize(keep);
    }
    stats_.count = static_cast<int>(particles_.size());
    stats_.stepMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

long long ParticleSystem::totalMass() const {
    long long s = 0;
    for (const Particle& p : particles_) s += mats_[p.cell.material].mass;
    return s;
}

double ParticleSystem::thermalEnergy() const {
    double s = 0;
    for (const Particle& p : particles_) s += static_cast<double>(mats_[p.cell.material].heatCapacity) * p.cell.temperature;
    return s;
}

double ParticleSystem::momentumX() const {
    double s = 0;
    for (const Particle& p : particles_) s += static_cast<double>(mats_[p.cell.material].mass) * p.vx;
    return s;
}

double ParticleSystem::momentumY() const {
    double s = 0;
    for (const Particle& p : particles_) s += static_cast<double>(mats_[p.cell.material].mass) * p.vy;
    return s;
}

} // namespace notita
