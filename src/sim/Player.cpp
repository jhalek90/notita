#include "Player.h"

#include "Bodies.h"
#include "Electric.h"
#include "GasGrid.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace notita {

const char* toolName(Tool t) {
    switch (t) {
    case Tool::Dig: return "Dig";
    case Tool::Jet: return "Jet";
    case Tool::Grapple: return "Grapple";
    case Tool::Blast: return "Blast";
    case Tool::Raft: return "Raft";
    case Tool::Freeze: return "Freeze";
    default: return "?";
    }
}

// ------------------------------------------------------------------ inventory

long long Inventory::total() const {
    long long s = 0;
    for (long long c : count_) s += c;
    return s;
}

void Inventory::add(MaterialId m, long long n) {
    if (creative || n <= 0) return;
    if (m >= count_.size()) count_.resize(static_cast<std::size_t>(m) + 1, 0);
    count_[m] += n;
}

bool Inventory::take(MaterialId m, long long n) {
    if (creative) return true;
    if (count(m) < n) return false;
    count_[m] -= n;
    return true;
}

long long Inventory::totalMass(const MaterialRegistry& mats) const {
    long long s = 0;
    for (std::size_t i = 0; i < count_.size() && i < mats.size(); ++i)
        s += count_[i] * mats[static_cast<MaterialId>(i)].mass;
    return s;
}

// ------------------------------------------------------------------ player

Player::Player(World& world, const MaterialRegistry& mats, std::uint64_t seed) : world_(world), mats_(mats), seed_(seed) {
    inv_.reset(mats.size());
    hardness_.assign(mats.size(), 0.0f);
    for (std::size_t i = 0; i < mats.size(); ++i) {
        const Material& m = mats[static_cast<MaterialId>(i)];
        float h = m.kind == MaterialKind::Solid ? std::clamp(m.density / 1000.0f, 0.2f, 10.0f) : 0.0f;
        if (auto it = m.extra.find("hardness"); it != m.extra.end()) h = std::max(0.0f, std::strtof(it->second.c_str(), nullptr));
        hardness_[i] = h;
    }
    health_ = params_.maxHealth;
}

Rng Player::rng(std::uint64_t salt) const {
    return Rng(Rng::hash(Rng::hash(seed_ ^ 0x51ED27A1B3C4D5E7ull, tick_), salt));
}

void Player::spawn(float x, float y) {
    x_ = x;
    y_ = y;
    vx_ = vy_ = 0.0f;
    health_ = params_.maxHealth;
    rope_ = false;
    grounded_ = false;
    groundBody_ = 0;
    airTicks_ = params_.coyoteTicks + 1; // no coyote jump out of thin air
    sinceHurt_ = params_.regenDelay;
    spawnX_ = x;
    spawnY_ = y;
}

int Player::halfWidthAt(int row) const {
    // a capsule: rounded head (1, 2 px), rounded feet (2 px)
    if (row == 0) return std::min(1, params_.halfWidth);
    if (row == 1 || row == params_.height - 1) return std::min(2, params_.halfWidth);
    return params_.halfWidth;
}

bool Player::covers(int wx, int wy) const {
    const int ix = static_cast<int>(std::floor(x_)), iy = static_cast<int>(std::floor(y_));
    const int row = wy - (iy - params_.height);
    if (row < 0 || row >= params_.height) return false;
    return std::abs(wx - ix) <= halfWidthAt(row);
}

float Player::massUnits() const {
    int cells = 0;
    for (int r = 0; r < params_.height; ++r) cells += 2 * halfWidthAt(r) + 1;
    return static_cast<float>(cells) * params_.density / 1000.0f;
}

bool Player::blocked(int x, int y) const {
    const Cell* c = world_.cell(x, y);
    if (!c) return true; // outside the world is a wall
    const MaterialKind k = mats_[c->material].kind;
    if (k == MaterialKind::Solid) return true;
    return k == MaterialKind::Powder && !ignorePowder_ && !flat_; // #483: from above, powder is pushed aside
}

bool Player::collides(int ix, int iy) const {
    for (int r = 0; r < params_.height; ++r) {
        const int wy = iy - params_.height + r, hw = halfWidthAt(r);
        for (int dx = -hw; dx <= hw; ++dx)
            if (blocked(ix + dx, wy)) return true;
    }
    return false;
}

bool Player::diggable(const Cell& c) const {
    if (c.flags & kFlagBody) return false; // a body's stamp: it belongs to the body this tick
    const Material& m = mats_[c.material];
    switch (m.kind) {
    case MaterialKind::Solid: return !m.anchor;
    case MaterialKind::Powder:
    case MaterialKind::Liquid: return true;
    default: return false;
    }
}

MaterialId Player::jetGas(MaterialId id) const {
    if (id == kEmpty || id >= mats_.size()) return kEmpty;
    const Material& m = mats_[id];
    if (m.kind == MaterialKind::Gas) return id;
    if (m.hot.valid && mats_[m.hot.into].kind == MaterialKind::Gas) return m.hot.into; // boils into it
    if (m.flammable() && mats_[m.burnsInto.into].kind == MaterialKind::Gas) return m.burnsInto.into;
    return kEmpty;
}

// Something moved into the player (sand fell on it, a body rose under it,
// a block was placed): push it out upwards first, then sideways. Buried in
// powder with nowhere to go, it wades through the powder this tick.
void Player::unstick() {
    ignorePowder_ = false;
    stuck_ = false;
    const int ix = static_cast<int>(std::floor(x_)), iy = static_cast<int>(std::floor(y_));
    if (!collides(ix, iy)) return;
    for (int k = 1; k <= 6; ++k)
        if (!collides(ix, iy - k)) { y_ -= static_cast<float>(k); return; }
    for (int k = 1; k <= 4; ++k)
        for (int d = -1; d <= 1; d += 2)
            if (!collides(ix + d * k, iy)) { x_ += static_cast<float>(d * k); return; }
    ignorePowder_ = true;
    if (!collides(ix, iy)) return;
    for (int k = 1; k <= 12; ++k)
        if (!collides(ix, iy - k)) { y_ -= static_cast<float>(k); return; }
    stuck_ = true; // inside solid: movement is blocked until something changes
}

void Player::sense() {
    const int ix = static_cast<int>(std::floor(x_)), iy = static_cast<int>(std::floor(y_));
    const int H = params_.height, hwMax = params_.halfWidth;
    int total = 0, liquid = 0;
    double dens = 0, visc = 0;
    float hottest = -kNoPoint, coldest = kNoPoint;
    bool flame = false;
    for (int r = -1; r <= H; ++r) {
        const int wy = iy - H + r;
        const int hw = (r >= 0 && r < H) ? halfWidthAt(r) : hwMax;
        for (int dx = -hw - 1; dx <= hw + 1; ++dx) {
            const Cell* c = world_.cell(ix + dx, wy);
            if (!c) continue;
            const Material& m = mats_[c->material];
            const bool inside = r >= 0 && r < H && std::abs(dx) <= hw;
            if (inside) {
                ++total;
                if (m.kind == MaterialKind::Liquid) {
                    ++liquid;
                    dens += m.density;
                    visc += m.viscosity;
                }
            }
            hottest = std::max(hottest, c->temperature);
            coldest = std::min(coldest, c->temperature);
            if (m.isFlame() || (c->flags & kFlagBurning)) flame = true;
        }
    }
    // #480: flames live on the gas grid now: burning gas in a cell the capsule covers burns too
    if (!flame && gas_ && gas_->params().enabled && gas_->fields().w > 0) {
        const GasFields& f = gas_->fields();
        const int i0 = (ix - hwMax - gas_->originX()) / kGasCell, i1 = (ix + hwMax - gas_->originX()) / kGasCell;
        const int j0 = (iy - H - gas_->originY()) / kGasCell, j1 = (iy - 1 - gas_->originY()) / kGasCell;
        for (int j = std::max(j0, 0); j <= std::min(j1, f.h - 1) && !flame; ++j)
            for (int i = std::max(i0, 0); i <= std::min(i1, f.w - 1) && !flame; ++i)
                flame = gas_->flameGlow()[static_cast<std::size_t>(j) * f.w + i] > 1.0f;
    }
    submerged_ = total > 0 ? static_cast<float>(liquid) / static_cast<float>(total) : 0.0f;
    liquidDensity_ = liquid > 0 ? static_cast<float>(dens / liquid) : 0.0f;
    liquidViscosity_ = liquid > 0 ? static_cast<float>(visc / liquid) : 0.0f;
    stats_.submerged = submerged_;
    stats_.hottest = hottest;

    float dmg = 0.0f;
    if (hottest > params_.hurtTemp) dmg += std::min(2.0f, (hottest - params_.hurtTemp) * params_.heatDamage);
    if (coldest < params_.coldTemp) dmg += std::min(2.0f, (params_.coldTemp - coldest) * params_.heatDamage);
    if (flame) dmg += params_.flameDamage;
    if (dmg > 0.0f) hurt(dmg);
}

void Player::hurt(float hp) {
    health_ -= hp;
    stats_.damage += hp;
    sinceHurt_ = 0;
}

void Player::moveX(float dx) {
    if (dx == 0.0f) return;
    const int dir = dx > 0 ? 1 : -1;
    float nx = x_ + dx;
    int cur = static_cast<int>(std::floor(x_));
    const int target = static_cast<int>(std::floor(nx));
    int iy = static_cast<int>(std::floor(y_));
    const bool canStep = !flat_ && (grounded_ || submerged_ > 0.3f || airTicks_ < 3); // #483: no ledges seen from above
    while (cur != target) {
        const int next = cur + dir;
        if (!collides(next, iy)) { cur = next; continue; }
        bool stepped = false;
        if (canStep)
            for (int k = 1; k <= params_.stepHeight; ++k)
                if (!collides(cur, iy - k) && !collides(next, iy - k)) {
                    iy -= k;
                    y_ -= static_cast<float>(k);
                    cur = next;
                    stepped = true;
                    break;
                }
        if (!stepped) {
            nx = dir > 0 ? static_cast<float>(cur) + 0.999f : static_cast<float>(cur);
            vx_ = 0.0f;
            break;
        }
    }
    x_ = nx;
}

void Player::moveY(float dy) {
    if (dy == 0.0f) return;
    const int dir = dy > 0 ? 1 : -1;
    float ny = y_ + dy;
    int cur = static_cast<int>(std::floor(y_));
    const int target = static_cast<int>(std::floor(ny));
    const int ix = static_cast<int>(std::floor(x_));
    while (cur != target) {
        const int next = cur + dir;
        if (!collides(ix, next)) { cur = next; continue; }
        ny = dir > 0 ? static_cast<float>(cur) + 0.999f : static_cast<float>(cur);
        vy_ = 0.0f;
        break;
    }
    y_ = ny;
}

// #483: walking through powder on a floor seen from above pushes it: every
// grain inside the capsule is moved to the first free pixel ahead of it
// (along the walk, else straight out of the nearer side), with a little of
// the player's speed so it slides on. Whole pixels move, so mass is kept.
void Player::shovePowder() {
    const int ix = static_cast<int>(std::floor(x_)), iy = static_cast<int>(std::floor(y_));
    const int H = params_.height;
    for (int r = 0; r < H; ++r) {
        const int wy = iy - H + r, hw = halfWidthAt(r);
        for (int wx = ix - hw; wx <= ix + hw; ++wx) {
            Cell* c = world_.cell(wx, wy);
            if (!c || (c->flags & kFlagBody) || mats_[c->material].kind != MaterialKind::Powder) continue;
            int dx = 0, dy = 0;
            if (std::abs(vx_) >= std::abs(vy_) && std::abs(vx_) > 0.05f) dx = vx_ > 0 ? 1 : -1;
            else if (std::abs(vy_) > 0.05f) dy = vy_ > 0 ? 1 : -1;
            else dx = wx >= ix ? 1 : -1;
            bool done = false;
            for (int k = 1; k <= params_.shoveReach + H && !done; ++k) {
                const int tx = wx + dx * k, ty = wy + dy * k;
                if (covers(tx, ty)) continue;
                Cell* t = world_.cell(tx, ty);
                if (!t) break;
                const MaterialKind tk = mats_[t->material].kind;
                if (tk == MaterialKind::Solid || (t->flags & kFlagBody)) break;
                if (tk == MaterialKind::Powder || tk == MaterialKind::Liquid) continue; // pushed through the pile
                if (k > params_.shoveReach + H) break;
                Cell grain = *c, air = *t;
                grain.vx = static_cast<std::int16_t>(std::clamp(vx_ * 0.5f, -1.5f, 1.5f) * kVelOne);
                grain.vy = static_cast<std::int16_t>(std::clamp(vy_ * 0.5f, -1.5f, 1.5f) * kVelOne);
                world_.setCell(tx, ty, grain);
                world_.setCell(wx, wy, air);
                done = true;
            }
        }
    }
}

void Player::step(const PlayerInput& in) {
    ++tick_;
    stats_.dug = stats_.placed = stats_.jetMass = 0;
    stats_.damage = 0.0f;
    stats_.blasted = false;
    const PlayerParams& P = params_;
    ++sinceHurt_;

    // #483: seen from above (the capsule's centre in a top-down region)
    flat_ = world_.gravity().flat(static_cast<int>(std::floor(x_)), static_cast<int>(std::floor(centreY())));

    unstick();
    sense();

    // electricity: current through the liquid around the player
    stats_.shock = 0.0f;
    if (electric_ && submerged_ > 0.0f && electric_->fieldChunks() > 0) {
        const int ix = static_cast<int>(std::floor(x_)), iy = static_cast<int>(std::floor(y_));
        float worst = 0.0f;
        for (int r = 0; r < P.height; ++r) {
            const int wy = iy - P.height + r, hw = halfWidthAt(r);
            for (int wx = ix - hw; wx <= ix + hw; ++wx) {
                const Cell* c = world_.cell(wx, wy);
                if (!c || mats_[c->material].kind != MaterialKind::Liquid) continue;
                worst = std::max(worst, electric_->currentAt(wx, wy));
            }
        }
        stats_.shock = worst;
        if (worst >= P.shockCurrent) hurt(P.shockDamage * std::min(worst, 3.0f));
    }

    // ---- forces (px/tick^2) ----
    float ax = 0.0f, ay = flat_ ? 0.0f : P.gravity;
    if (submerged_ > 0.0f && !flat_) ay -= P.gravity * submerged_ * liquidDensity_ / P.density; // buoyancy

    // the gas grid: wind drag and pressure, sampled over the cells the capsule covers
    stats_.windX = stats_.windY = 0.0f;
    // the cloak: holding jump while falling in the open spreads it, so the air holds the player up
    // much more (vertically); a gentle updraft then carries it
    gliding_ = !flat_ && in.jump && !grounded_ && !rope_ && submerged_ < 0.2f && (gliding_ || vy_ > -0.5f); // opens at the top of a jump, stays open
    const float dragY = gliding_ ? P.glideDrag : P.gasDrag;
    bool windSampled = false;
    if (gas_ && gas_->params().enabled && gas_->fields().w > 0) {
        const GasFields& f = gas_->fields();
        const int ix = static_cast<int>(std::floor(x_)), iy = static_cast<int>(std::floor(y_));
        const int i0 = (ix - P.halfWidth - gas_->originX()) / kGasCell, i1 = (ix + P.halfWidth - gas_->originX()) / kGasCell;
        const int j0 = (iy - P.height - gas_->originY()) / kGasCell, j1 = (iy - 1 - gas_->originY()) / kGasCell;
        float wx = 0, wy = 0;
        int n = 0;
        auto inGrid = [&](int i, int j) { return i >= 0 && j >= 0 && i < f.w && j < f.h; };
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                if (!inGrid(i, j) || f.open[j * f.w + i] <= 0.0f) continue;
                wx += f.uc(i, j) * kGasCell;
                wy += f.vc(i, j) * kGasCell;
                ++n;
            }
        if (n > 0) {
            wx /= static_cast<float>(n);
            wy /= static_cast<float>(n);
            stats_.windX = wx;
            stats_.windY = wy;
            const float air = 1.0f - submerged_;
            ax += P.gasDrag * (wx - vx_) * air;
            ay += dragY * (wy - vy_) * air;
            windSampled = true;
        }
        // pressure gradient across the capsule (open cells on both sides only)
        auto meanP = [&](int ia, int ib, int ja, int jb, bool& ok) {
            float s = 0;
            int m = 0;
            for (int j = ja; j <= jb; ++j)
                for (int i = ia; i <= ib; ++i)
                    if (inGrid(i, j) && f.open[j * f.w + i] > 0.0f) { s += f.p[j * f.w + i]; ++m; }
            ok = m > 0;
            return m > 0 ? s / static_cast<float>(m) : 0.0f;
        };
        bool okL, okR, okT, okB;
        const float pl = meanP(i0 - 1, i0 - 1, j0, j1, okL), pr = meanP(i1 + 1, i1 + 1, j0, j1, okR);
        const float pt = meanP(i0, i1, j0 - 1, j0 - 1, okT), pb = meanP(i0, i1, j1 + 1, j1 + 1, okB);
        if (okL && okR) ax -= P.gasPush * (pr - pl) / static_cast<float>((i1 - i0 + 2) * kGasCell);
        if (okT && okB) ay -= P.gasPush * (pb - pt) / static_cast<float>((j1 - j0 + 2) * kGasCell);
    }

    if (!windSampled && !flat_) ay -= dragY * vy_ * (1.0f - submerged_); // still air (no grid)

    // ---- input ----
    const bool jumpEdge = in.jump && !wasJump_;
    wasJump_ = in.jump;
    if (flat_) {
        // #483: walking on a floor seen from above, in 8 directions (diagonals as fast as
        // straight), wading slower through liquid; no jumping, swimming or stepping up
        float mx = std::clamp(in.move, -1.0f, 1.0f), my = std::clamp(in.moveY, -1.0f, 1.0f);
        const float len = std::sqrt(mx * mx + my * my);
        if (len > 1.0f) { mx /= len; my /= len; }
        if (mx != 0.0f) facing_ = mx > 0 ? 1 : -1;
        const float speed = P.walkSpeed * (submerged_ > 0.3f ? 0.6f : 1.0f);
        vx_ += std::clamp(mx * speed - vx_, -P.groundAccel, P.groundAccel);
        vy_ += std::clamp(my * speed - vy_, -P.groundAccel, P.groundAccel);
    } else {
    if (in.move != 0.0f) facing_ = in.move > 0 ? 1 : -1;
    {
        const float target = std::clamp(in.move, -1.0f, 1.0f) * P.walkSpeed * (submerged_ > 0.5f ? 0.6f : 1.0f);
        const float accel = grounded_ ? P.groundAccel : (submerged_ > 0.3f ? 0.12f : P.airAccel);
        if (in.move != 0.0f || grounded_) vx_ += std::clamp(target - vx_, -accel, accel);
    }
    if (jumpEdge && !rope_ && submerged_ < 0.6f && (grounded_ || airTicks_ <= P.coyoteTicks)) {
        vy_ = -P.jumpSpeed;
        if (groundBody_ && bodies_) bodies_->applyImpulse(groundBody_, x_, y_, 0.0f, massUnits() * P.jumpSpeed);
        airTicks_ = P.coyoteTicks + 1;
        grounded_ = false;
    }
    if (submerged_ > 0.2f) {
        if (in.jump) ay -= P.swimAccel;
        if (in.down) ay += P.swimAccel * 0.6f;
    }
    }
    vx_ += ax;
    vy_ += ay;

    jet(in);

    if (submerged_ > 0.0f) {
        const float k = std::clamp(P.liquidDrag * submerged_ * (1.0f + 3.0f * liquidViscosity_), 0.0f, 0.6f);
        vx_ *= 1.0f - k;
        vy_ *= 1.0f - k;
    }
    rope(in);
    const float sp = std::sqrt(vx_ * vx_ + vy_ * vy_);
    if (sp > P.maxSpeed) { vx_ *= P.maxSpeed / sp; vy_ *= P.maxSpeed / sp; }

    // ---- move: carried sideways by the body under the feet (its stamp already moved; unstick lifts us) ----
    float carry = 0.0f;
    if (groundBody_ && bodies_) {
        float bvx = 0, bvy = 0;
        if (bodies_->velocityAt(groundBody_, x_, y_, bvx, bvy)) carry = bvx;
        // our weight presses on it
        bodies_->applyImpulse(groundBody_, x_, y_, 0.0f, massUnits() * P.gravity);
    }
    const float landing = vy_;
    const bool wasGrounded = grounded_;
    moveX(vx_ + carry);
    moveY(vy_);
    if (flat_) shovePowder();

    const int ix = static_cast<int>(std::floor(x_)), iy = static_cast<int>(std::floor(y_));
    grounded_ = flat_ || collides(ix, iy + 1); // #483: on a floor seen from above the player always stands
    groundBody_ = 0;
    if (flat_) {
        airTicks_ = 0;
    } else if (grounded_) {
        if (!wasGrounded && landing > P.landSpeed) hurt((landing - P.landSpeed) * P.landDamage);
        airTicks_ = 0;
        if (bodies_)
            for (int dx = -2; dx <= 2 && !groundBody_; ++dx) {
                const Cell* c = world_.cell(ix + dx, iy);
                if (c && (c->flags & kFlagBody)) groundBody_ = bodies_->bodyAt(ix + dx, iy);
            }
    } else {
        ++airTicks_;
    }

    useTools(in);

    if (sinceHurt_ > P.regenDelay) health_ = std::min(P.maxHealth, health_ + P.regen);
    if (health_ <= 0.0f) {
        ++stats_.deaths;
        const int deaths = stats_.deaths;
        spawn(spawnX_, spawnY_);
        stats_.deaths = deaths;
    }
}

void Player::aimPoint(const PlayerInput& in, float reach, float& tx, float& ty) const {
    const float cx = x_, cy = centreY();
    float dx = in.aimX - cx, dy = in.aimY - cy;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d > reach && d > 0.0f) { dx *= reach / d; dy *= reach / d; }
    tx = cx + dx;
    ty = cy + dy;
}

namespace {
struct Px { int x, y; };
void brushCells(int cx, int cy, int r, BrushShape shape, std::vector<Px>& out) {
    out.clear();
    r = std::max(0, r);
    const int r2 = r * r + r;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            if (shape == BrushShape::Square || dx * dx + dy * dy <= r2) out.push_back({cx + dx, cy + dy});
}
template <class T> void shuffle(std::vector<T>& v, Rng& g) {
    for (std::size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[g.below(static_cast<std::uint32_t>(i))]);
}
} // namespace

void Player::useTools(const PlayerInput& in) {
    float tx, ty;
    aimPoint(in, params_.reach, tx, ty);
    if (in.place) place(in, tx, ty);
    switch (in.tool) {
    case Tool::Dig:
        if (in.use) dig(in, tx, ty);
        break;
    case Tool::Grapple:
        if (in.usePressed) grapple(in);
        break;
    case Tool::Blast:
        if (in.usePressed && (inv_.creative || inv_.charges > 0)) {
            float bx, by;
            aimPoint(in, params_.blastReach, bx, by);
            blastAt(gas_, bodies_, this, static_cast<int>(std::floor(bx)), static_cast<int>(std::floor(by)), params_.blastRadius,
                    params_.blastPower);
            if (!inv_.creative) --inv_.charges;
            stats_.blasted = true;
        }
        break;
    case Tool::Raft:
        if (in.usePressed) raft(in, tx, ty);
        break;
    case Tool::Freeze:
        if (in.use) {
            // a cooling brush that stops at freezeFloor: cold enough to freeze water, not to hurt
            std::vector<Px> cells;
            const int r = std::max(1, in.brushRadius);
            const int cx = static_cast<int>(std::floor(tx)), cy = static_cast<int>(std::floor(ty));
            brushCells(cx, cy, r, in.shape, cells);
            const float r2 = static_cast<float>(r * r + r);
            for (const Px& p : cells) {
                const Cell* c = world_.cell(p.x, p.y);
                if (!c || c->temperature <= params_.freezeFloor) continue;
                const float d2 = static_cast<float>((p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy)) / r2;
                const float cool = params_.freezeRate * (1.0f - 0.75f * std::min(d2, 1.0f));
                world_.addHeat(p.x, p.y, -std::min(cool, c->temperature - params_.freezeFloor));
            }
        }
        break;
    default: break; // the jet acts with the forces
    }
}

void Player::dig(const PlayerInput& in, float tx, float ty) {
    std::vector<Px> cells;
    brushCells(static_cast<int>(std::floor(tx)), static_cast<int>(std::floor(ty)), in.brushRadius, in.shape, cells);
    Rng g = rng(1);
    shuffle(cells, g);
    int n = 0;
    for (const Px& p : cells) {
        if (n >= params_.digRate || !inv_.room()) break;
        const Cell* c = world_.cell(p.x, p.y);
        if (!c || !diggable(*c)) continue;
        const float chance = 1.0f / (1.0f + params_.digHardness * hardness(c->material));
        if (g.unit() >= chance) continue;
        inv_.add(c->material);
        world_.setCell(p.x, p.y, Cell{}); // air at the ambient temperature
        ++n;
    }
    stats_.dug = n;
}

void Player::place(const PlayerInput& in, float tx, float ty) {
    const MaterialId m = in.material;
    if (m == kEmpty || m >= mats_.size()) return;
    const MaterialKind k = mats_[m].kind;
    const bool solidish = k == MaterialKind::Solid || k == MaterialKind::Powder || k == MaterialKind::Liquid;
    if (!solidish && !inv_.creative) return; // gases and flames only in creative
    std::vector<Px> cells;
    brushCells(static_cast<int>(std::floor(tx)), static_cast<int>(std::floor(ty)), in.brushRadius, in.shape, cells);
    Rng g = rng(2);
    shuffle(cells, g);
    int n = 0;
    for (const Px& p : cells) {
        if (n >= params_.placeRate) break;
        const Cell* c = world_.cell(p.x, p.y);
        if (!c || c->material != kEmpty || covers(p.x, p.y)) continue;
        if (!inv_.take(m)) break;
        Cell cell = mats_[m].spawn(mats_[m].shadeAt(p.x, p.y));
        if (k == MaterialKind::Solid && in.placePinned) cell.aux = kAuxPinned;
        world_.setCell(p.x, p.y, cell);
        ++n;
    }
    stats_.placed = n;
}

void Player::jet(const PlayerInput& in) {
    if (!in.use || in.tool != Tool::Jet || !gas_ || !gas_->params().enabled) return;
    auto usable = [&](MaterialId m) {
        const MaterialId g = jetGas(m);
        return g != kEmpty && gas_->takesGas(g) && inv_.has(m, params_.jetRate);
    };
    MaterialId prop = kEmpty;
    if (usable(in.material)) prop = in.material;
    else if (inv_.creative) prop = usable(mats_.idOf("water")) ? mats_.idOf("water") : kEmpty;
    else
        for (std::size_t i = 1; i < mats_.size() && prop == kEmpty; ++i)
            if (inv_.count(static_cast<MaterialId>(i)) > 0 && usable(static_cast<MaterialId>(i))) prop = static_cast<MaterialId>(i);
    if (prop == kEmpty) return;
    const MaterialId gas = jetGas(prop);
    const float cx = x_, cy = centreY();
    float dx = in.aimX - cx, dy = in.aimY - cy;
    float d = std::sqrt(dx * dx + dy * dy);
    if (d < 0.5f) { dx = 0.0f; dy = 1.0f; d = 1.0f; }
    dx /= d;
    dy /= d;
    // the nozzle just outside the capsule, towards the aim
    const float out = std::abs(dy) * (static_cast<float>(params_.height) * 0.5f + 1.5f) + std::abs(dx) * (static_cast<float>(params_.halfWidth) + 2.0f);
    const int nx = static_cast<int>(std::floor(cx + dx * out)), ny = static_cast<int>(std::floor(cy + dy * out));
    if (!inv_.take(prop, params_.jetRate)) return;
    const Material& pm = mats_[prop];
    const int mass = pm.mass * params_.jetRate;
    const float temp = (pm.hot.valid && pm.hot.into == gas) ? pm.hot.point + 5.0f : std::max(mats_[gas].spawnTemp, 200.0f);
    gas_->jet(nx, ny, gas, mass, temp, dx * params_.jetGasSpeed, dy * params_.jetGasSpeed);
    // recoil: the gas leaves at jetExhaust, the player goes the other way
    const float a = params_.jetExhaust * static_cast<float>(mass) / massUnits();
    vx_ -= dx * a;
    vy_ -= dy * a;
    stats_.jetMass = mass;
    stats_.jetMassTotal += mass;
}

void Player::grapple(const PlayerInput& in) {
    if (rope_) { rope_ = false; return; }
    const float cx = x_, cy = centreY();
    float dx = in.aimX - cx, dy = in.aimY - cy;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0f) return;
    dx /= len;
    dy /= len;
    for (float t = 0.5f; t <= params_.ropeMax; t += 0.5f) {
        const int px = static_cast<int>(std::floor(cx + dx * t)), py = static_cast<int>(std::floor(cy + dy * t));
        if (covers(px, py)) continue;
        const Cell* c = world_.cell(px, py);
        if (!c) return;
        if (mats_[c->material].kind != MaterialKind::Solid) continue;
        rope_ = true;
        ropeX_ = static_cast<float>(px) + 0.5f;
        ropeY_ = static_cast<float>(py) + 0.5f;
        ropeLen_ = std::max(6.0f, t);
        ropeBody_ = bodies_ ? bodies_->bodyAt(px, py) : 0;
        if (ropeBody_) {
            const RigidBody* b = bodies_->body(ropeBody_);
            const float rx = ropeX_ - b->x, ry = ropeY_ - b->y;
            ropeLx_ = b->c * rx + b->s * ry;
            ropeLy_ = -b->s * rx + b->c * ry;
        }
        return;
    }
}

void Player::rope(const PlayerInput& in) {
    if (!rope_) return;
    if (ropeBody_ && bodies_) {
        if (const RigidBody* b = bodies_->body(ropeBody_)) {
            ropeX_ = b->x + b->c * ropeLx_ - b->s * ropeLy_;
            ropeY_ = b->y + b->s * ropeLx_ + b->c * ropeLy_;
        } else {
            ropeBody_ = 0; // it froze back into pixels where it lies
        }
    }
    if (!ropeBody_) {
        const Cell* c = world_.cell(static_cast<int>(std::floor(ropeX_)), static_cast<int>(std::floor(ropeY_)));
        if (!c || mats_[c->material].kind != MaterialKind::Solid) { rope_ = false; return; } // dug out, melted, burnt
    }
    if (in.jump) ropeLen_ = std::max(6.0f, ropeLen_ - params_.ropeReel);
    if (in.down) ropeLen_ = std::min(params_.ropeMax, ropeLen_ + params_.ropeReel);
    // inextensible: where this tick's velocity would take the centre, pulled back onto the circle
    const float px = x_ + vx_, py = centreY() + vy_;
    const float dx = px - ropeX_, dy = py - ropeY_;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d <= ropeLen_ || d <= 0.0f) return;
    const float nx = dx / d, ny = dy / d;
    const float excess = std::min(d - ropeLen_, 4.0f);
    vx_ -= nx * excess;
    vy_ -= ny * excess;
    if (ropeBody_ && bodies_) bodies_->applyImpulse(ropeBody_, ropeX_, ropeY_, nx * massUnits() * excess, ny * massUnits() * excess);
}

void Player::raft(const PlayerInput& in, float tx, float ty) {
    MaterialId m = in.material;
    if (m == kEmpty || mats_[m].kind != MaterialKind::Solid) m = mats_.idOf("timber");
    if (m == kEmpty || mats_[m].kind != MaterialKind::Solid) return;
    const int W = std::max(1, params_.raftW), H = std::max(1, params_.raftH);
    if (!inv_.has(m, static_cast<long long>(W) * H)) return;
    const int x0 = static_cast<int>(std::floor(tx)) - W / 2, y0 = static_cast<int>(std::floor(ty)) - H / 2;
    // the block must be in open air with nothing solid around it, or it would join the terrain
    for (int y = y0 - 1; y <= y0 + H; ++y)
        for (int x = x0 - 1; x <= x0 + W; ++x) {
            const Cell* c = world_.cell(x, y);
            if (!c) return;
            const bool inside = x >= x0 && x < x0 + W && y >= y0 && y < y0 + H;
            if (inside && (c->material != kEmpty || covers(x, y))) return;
            if (!inside && mats_[c->material].kind == MaterialKind::Solid) return;
        }
    if (!inv_.take(m, static_cast<long long>(W) * H)) return;
    for (int y = y0; y < y0 + H; ++y)
        for (int x = x0; x < x0 + W; ++x) world_.setCell(x, y, mats_[m].spawn(mats_[m].shadeAt(x, y)));
    stats_.placed += W * H;
    if (bodies_) bodies_->releaseClump(x0 + W / 2, y0 + H / 2);
}

void Player::explode(int x, int y, int radius, float power) {
    const float cx = x_, cy = centreY();
    const float dx = cx - static_cast<float>(x), dy = cy - static_cast<float>(y);
    const float d = std::sqrt(dx * dx + dy * dy);
    const float r = static_cast<float>(radius) * 2.5f;
    if (d > r || power <= 0.0f) return;
    const float fall = 1.0f - d / (r + 1.0f);
    float nx = d > 0.5f ? dx / d : 0.0f, ny = (d > 0.5f ? dy / d : -1.0f) - 0.6f;
    const float len = std::sqrt(nx * nx + ny * ny);
    nx /= len;
    ny /= len;
    const float dv = params_.blastKick * power * fall;
    vx_ += nx * dv;
    vy_ += ny * dv;
    grounded_ = false;
    airTicks_ = params_.coyoteTicks + 1;
}

void blastAt(GasGrid* gas, BodySystem* bodies, Player* player, int x, int y, int radius, float power) {
    if (gas) gas->explode(x, y, radius, power);
    if (bodies) bodies->explode(x, y, radius, power);
    if (player) player->explode(x, y, radius, power);
}

PlayerState Player::state() const {
    PlayerState st;
    st.x = x_; st.y = y_; st.vx = vx_; st.vy = vy_;
    st.spawnX = spawnX_; st.spawnY = spawnY_; st.health = health_;
    st.tick = tick_;
    st.airTicks = airTicks_; st.sinceHurt = sinceHurt_; st.facing = facing_;
    st.grounded = grounded_; st.stuck = stuck_; st.ignorePowder = ignorePowder_;
    st.wasJump = wasJump_; st.gliding = gliding_;
    st.rope = rope_ && ropeBody_ == 0;
    st.ropeX = ropeX_; st.ropeY = ropeY_; st.ropeLen = ropeLen_;
    return st;
}

void Player::setState(const PlayerState& st) {
    x_ = st.x; y_ = st.y; vx_ = st.vx; vy_ = st.vy;
    spawnX_ = st.spawnX; spawnY_ = st.spawnY; health_ = st.health;
    tick_ = st.tick;
    airTicks_ = st.airTicks; sinceHurt_ = st.sinceHurt; facing_ = st.facing;
    grounded_ = st.grounded; stuck_ = st.stuck; ignorePowder_ = st.ignorePowder;
    wasJump_ = st.wasJump; gliding_ = st.gliding;
    rope_ = st.rope; ropeBody_ = 0;
    ropeX_ = st.ropeX; ropeY_ = st.ropeY; ropeLen_ = st.ropeLen;
    groundBody_ = 0;
}

} // namespace notita
