#include "Showcase.h"

#include "Bodies.h"
#include "ChunkCodec.h"
#include "Rng.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace notita {

namespace {

// 3x5 pixel font for the room signs (one row per string, '#' = lit)
struct Glyph { char c; const char* rows[5]; };
constexpr Glyph kFont[] = {
    {'A', {"###", "#.#", "###", "#.#", "#.#"}}, {'B', {"##.", "#.#", "##.", "#.#", "##."}},
    {'C', {"###", "#..", "#..", "#..", "###"}}, {'D', {"##.", "#.#", "#.#", "#.#", "##."}},
    {'E', {"###", "#..", "##.", "#..", "###"}}, {'F', {"###", "#..", "##.", "#..", "#.."}},
    {'G', {"###", "#..", "#.#", "#.#", "###"}}, {'H', {"#.#", "#.#", "###", "#.#", "#.#"}},
    {'I', {"###", ".#.", ".#.", ".#.", "###"}}, {'L', {"#..", "#..", "#..", "#..", "###"}},
    {'M', {"#.#", "###", "###", "#.#", "#.#"}}, {'N', {"##.", "#.#", "#.#", "#.#", "#.#"}},
    {'O', {"###", "#.#", "#.#", "#.#", "###"}}, {'P', {"###", "#.#", "###", "#..", "#.."}},
    {'R', {"##.", "#.#", "##.", "#.#", "#.#"}}, {'S', {"###", "#..", "###", "..#", "###"}},
    {'T', {"###", ".#.", ".#.", ".#.", ".#."}}, {'U', {"#.#", "#.#", "#.#", "#.#", "###"}},
    {'V', {"#.#", "#.#", "#.#", "#.#", ".#."}}, {'W', {"#.#", "#.#", "###", "###", "#.#"}},
    {'Y', {"#.#", "#.#", ".#.", ".#.", ".#."}}, {'K', {"#.#", "##.", "#..", "##.", "#.#"}},
};

const Glyph* glyph(char c) {
    for (const Glyph& g : kFont)
        if (g.c == c) return &g;
    return nullptr;
}

std::uint32_t mix(std::uint32_t h) {
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return h;
}
std::uint32_t hash2(int x, int y) { return mix(static_cast<std::uint32_t>(x) * 0x9E3779B1u ^ mix(static_cast<std::uint32_t>(y) + 0x632BE5ABu)); }

// Layout (world pixels, +y down). The ground's first row is y = 0.
constexpr int kBoxX0 = -1100, kBoxX1 = 1500, kBoxY0 = -330, kBoxY1 = 300;
constexpr int kBlend = 220;             // px over which the generated surface comes back outside the box
constexpr int kBX0 = -700, kBX1 = 700;   // building outer walls
constexpr int kSlab = 12;
constexpr int kGF = -96;                 // ground floor interior top (floor top is y 0)
constexpr int kF1Floor = -108;           // first floor: floor top
constexpr int kF1 = -204;                // first floor interior top
constexpr int kRoof = -216;              // roof slab top

} // namespace

// Paints the plan with the scene's materials; everything it doesn't touch is
// left to the generator.
struct ShowcasePainter {
    Showcase& s;
    const MaterialRegistry& mats;

    MaterialId id(const char* n, const char* fallback = "slate") const {
        MaterialId m = mats.idOf(n);
        if (m == kEmpty && fallback) m = mats.idOf(fallback);
        return m;
    }

    enum Look : std::uint8_t { Hash, Formwork, GrainH, GrainV, Panel };

    std::uint8_t shadeFor(MaterialId m, Look look, int x, int y) const {
        const std::uint32_t h = hash2(x, y);
        switch (look) {
        case Formwork: {
            // cast concrete: big form panels with dark joints and tie holes, a little streaking
            const int px = ((x % 48) + 48) % 48, py = ((y % 32) + 32) % 32;
            if (px == 0 || py == 0) return static_cast<std::uint8_t>(18 + (h & 15));
            if ((px == 12 || px == 36) && (py == 8 || py == 24)) return static_cast<std::uint8_t>(6 + (h & 7));
            const int streak = static_cast<int>(mix(static_cast<std::uint32_t>(x) * 0x51ED27u) & 31);
            return static_cast<std::uint8_t>(std::clamp(92 + streak + static_cast<int>(h % 70), 1, 255));
        }
        case GrainH: case GrainV: {
            // timber grain: streaks along the beam, knots now and then
            const int along = look == GrainH ? x : y, across = look == GrainH ? y : x;
            const int streak = static_cast<int>(mix(static_cast<std::uint32_t>(across) * 0x2545F491u) & 63);
            const int wave = static_cast<int>(12.0f * std::sin(static_cast<float>(along) * 0.21f + static_cast<float>(across) * 1.7f));
            int v = 70 + streak + wave + static_cast<int>(h & 31);
            if ((mix(static_cast<std::uint32_t>(along / 9) * 0x1B873593u ^ static_cast<std::uint32_t>(across / 3)) & 31) == 0) v -= 60;
            return static_cast<std::uint8_t>(std::clamp(v, 1, 255));
        }
        case Panel: return static_cast<std::uint8_t>(110 + (h & 63));
        default: (void)m; return 0;
        }
    }

    void put(int x, int y, MaterialId m, Showcase::Hold hold = Showcase::Pinned, Look look = Hash) {
        if (x < s.x0_ || y < s.y0_ || x >= s.x0_ + s.w_ || y >= s.y0_ + s.h_) return;
        const std::size_t i = static_cast<std::size_t>(y - s.y0_) * static_cast<std::size_t>(s.w_) + static_cast<std::size_t>(x - s.x0_);
        s.mat_[i] = m;
        s.hold_[i] = hold;
        s.shade_[i] = look == Hash ? 0 : std::max<std::uint8_t>(1, shadeFor(m, look, x, y));
    }
    void rect(MaterialId m, int x0, int y0, int x1, int y1, Showcase::Hold hold = Showcase::Pinned, Look look = Hash) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) put(x, y, m, hold, look);
    }
    void air(int x0, int y0, int x1, int y1) { rect(kEmpty, x0, y0, x1, y1); }
    void concrete(int x0, int y0, int x1, int y1) { rect(id("concrete"), x0, y0, x1, y1, Showcase::Pinned, Formwork); }
    void beam(int x0, int y0, int x1, int y1, Showcase::Hold hold = Showcase::Pinned) {
        rect(id("timber"), x0, y0, x1, y1, hold, (x1 - x0) >= (y1 - y0) ? GrainH : GrainV);
    }
    void circle(MaterialId m, int cx, int cy, int r, Showcase::Hold hold = Showcase::Pinned) {
        for (int y = cy - r; y <= cy + r; ++y)
            for (int x = cx - r; x <= cx + r; ++x)
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) put(x, y, m, hold);
    }
    void ellipse(MaterialId m, int cx, int cy, int rx, int ry) {
        for (int y = cy - ry; y <= cy + ry; ++y)
            for (int x = cx - rx; x <= cx + rx; ++x) {
                const float fx = static_cast<float>(x - cx) / static_cast<float>(rx), fy = static_cast<float>(y - cy) / static_cast<float>(ry);
                if (fx * fx + fy * fy <= 1.0f) put(x, y, m);
            }
    }
    // text with its top-left at (x, y); each font pixel is `scale` px
    int textWidth(const char* t, int scale) const { return static_cast<int>(std::strlen(t)) * 4 * scale - scale; }
    void text(MaterialId m, const char* t, int x, int y, int scale) {
        for (; *t; ++t, x += 4 * scale) {
            const Glyph* g = glyph(*t);
            if (!g) continue;
            for (int r = 0; r < 5; ++r)
                for (int c = 0; c < 3; ++c)
                    if (g->rows[r][c] == '#') rect(m, x + c * scale, y + r * scale, x + c * scale + scale - 1, y + r * scale + scale - 1);
        }
    }
    void textCentred(MaterialId m, const char* t, int cx, int y, int scale) { text(m, t, cx - textWidth(t, scale) / 2, y, scale); }

    // A row of ceiling lamps: a volt cell, a two-row copper wire with lamps in
    // series hanging under the girder, and a ground rod (always on).
    void lampLine(int xa, int xb, int top, int lamps) {
        const int wy = top + 7;
        rect(id("volt_cell"), xa, wy - 3, xa + 5, wy + 4);
        rect(id("ground_rod"), xb - 5, wy - 3, xb, wy + 4);
        rect(id("copper"), xa + 6, wy, xb - 6, wy + 1);
        const int span = xb - xa - 12;
        for (int k = 0; k < lamps; ++k) {
            const int cx = xa + 6 + span * (2 * k + 1) / (2 * lamps);
            rect(id("lamp"), cx - 8, wy - 2, cx + 7, wy + 3);
        }
    }
    // A timber girder under a slab, with small brackets at both walls.
    void girder(int x0, int x1, int top) {
        beam(x0, top, x1, top + 3);
        for (int k = 0; k < 5; ++k) {
            beam(x0, top + 4 + k, x0 + 4 - k, top + 4 + k);
            beam(x1 - 4 + k, top + 4 + k, x1, top + 4 + k);
        }
    }
    void sign(const char* t, int cx, int slabTop) { textCentred(id("sign_light", "lamp_lit"), t, cx, slabTop + 1, 2); }
    // A free-standing sign board on two timber posts, standing on the ground.
    void signBoard(const char* t, int cx) {
        const int w = textWidth(t, 2);
        const int x0 = cx - w / 2 - 4, x1 = cx + (w - w / 2) + 3;
        beam(x0 + 4, -24, x0 + 6, -1);
        beam(x1 - 6, -24, x1 - 4, -1);
        concrete(x0, -40, x1, -25);
        sign(t, cx, -39);
    }
    // A street lamp: a timber pole with a little circuit on a concrete cap.
    void lampPost(int x) {
        beam(x - 1, -44, x + 1, -1);
        concrete(x - 11, -46, x + 11, -45);
        rect(id("volt_cell"), x - 11, -54, x - 6, -47);
        rect(id("lamp"), x - 5, -52, x + 5, -47);
        rect(id("ground_rod"), x + 6, -54, x + 11, -47);
    }
    void tree(int x, int h, int r) {
        beam(x - 2, -h, x + 2, -1);
        beam(x - 1 - r / 2, -h + r / 2, x - 3, -h + r / 2 + 1);
        ellipse(id("turf"), x, -h - r / 3, r, r * 2 / 3);
    }
    void tunnel(const int (*pts)[2], int n, int r) {
        for (int i = 0; i + 1 < n; ++i) {
            const float dx = static_cast<float>(pts[i + 1][0] - pts[i][0]), dy = static_cast<float>(pts[i + 1][1] - pts[i][1]);
            const int steps = static_cast<int>(std::sqrt(dx * dx + dy * dy)) + 1;
            for (int k = 0; k <= steps; ++k) {
                const float t = static_cast<float>(k) / static_cast<float>(steps);
                circle(kEmpty, pts[i][0] + static_cast<int>(dx * t), pts[i][1] + static_cast<int>(dy * t), r);
            }
        }
    }
    void room(const char* name, int x0, int y0, int x1, int y1) { s.rooms_.push_back({name, x0, y0, x1, y1}); }

    void paint();
};

void ShowcasePainter::paint() {
    using H = Showcase::Hold;
    const MaterialId turf = id("turf"), loam = id("loam"), slate = id("slate"), sand = id("sand"), grit = id("cinder_grit");
    const MaterialId water = id("water"), oil = id("lamp_oil", "water"), brine = id("brine", "water"), glass = id("glass");
    const MaterialId copper = id("copper"), iron = id("iron"), brick = id("firebrick", "basalt");
    const MaterialId hydrogen = id("hydrogen", nullptr), timber = id("timber");

    // ---- ground: flat meadow over the whole site, slate below ----
    for (int x = kBoxX0; x <= kBoxX1; ++x) {
        const int loamDepth = 12 + static_cast<int>(mix(static_cast<std::uint32_t>(x / 23) * 0x27D4EB2Fu) % 6);
        rect(turf, x, 0, x, 2);
        rect(loam, x, 3, x, loamDepth);
        rect(slate, x, loamDepth + 1, x, kBoxY1);
    }

    // ---- the building shell ----
    concrete(kBX0, kRoof, kBX1, 11);            // filled first, rooms carved out below
    const int rooms[5][2] = {{-690, -531}, {-522, -203}, {-194, 194}, {203, 522}, {531, 690}};
    for (const auto& r : rooms) {
        air(r[0], kGF, r[1], -1);
        air(r[0], kF1, r[1], kF1Floor - 1 + 0); // first floor interior: -204 .. -109
    }
    // doors (32 px tall: the player is 20)
    auto door = [&](int x0, int x1, int floorTop) { air(x0, floorTop - 32, x1, floorTop - 1); };
    door(kBX0, kBX0 + 9, 0);            // west: out to the cave
    door(kBX1 - 9, kBX1, 0);            // east: out to the course
    door(kBX1 - 9, kBX1, kF1Floor);     // east, first floor (from the jet tower)
    door(-530, -523, 0);                // silo <-> fluids
    door(-202, -195, 0);                // fluids <-> lobby
    door(195, 202, 0);                  // lobby <-> electrical
    door(523, 530, 0);                  // electrical <-> furnace
    door(-202, -195, kF1Floor);         // gallery <-> gas room
    door(195, 202, kF1Floor);           // gallery <-> demolition
    door(523, 530, kF1Floor);           // demolition <-> flue room
    // roof parapets
    concrete(kBX0, kRoof - 10, kBX0 + 9, kRoof - 1);
    concrete(kBX1 - 9, kRoof - 10, kBX1, kRoof - 1);

    // girders under every slab (split where the silo, the stairwell and the flue pass)
    girder(-550, -531, kGF);
    girder(-522, -203, kGF);
    girder(-194, 12, kGF);
    girder(94, 194, kGF);
    girder(203, 522, kGF);
    girder(531, 595, kGF);
    girder(625, 690, kGF);
    girder(-550, -531, kF1);
    girder(-522, -203, kF1);
    girder(-194, 194, kF1);
    girder(203, 522, kF1);
    girder(531, 595, kF1);
    girder(625, 690, kF1);

    // ceiling lights (real circuits: volt cell -> wire -> lamps -> ground rod)
    lampLine(-518, -207, kGF, 3);   // fluids
    lampLine(-190, 10, kGF, 2);     // lobby (clear of the stairs)
    lampLine(207, 518, kGF, 2);     // electrical
    lampLine(534, 592, kGF, 1);     // furnace
    lampLine(-518, -207, kF1, 2);   // gas room
    lampLine(-190, 190, kF1, 3);    // gallery
    lampLine(207, 518, kF1, 2);     // demolition
    lampLine(628, 687, kF1, 1);     // flue room

    // ---- signs in the slabs ----
    sign("FLUIDS", -362, kSlab * 0 + kF1Floor);
    sign("LOBBY", -110, kF1Floor);
    sign("POWER", 362, kF1Floor);
    sign("FIRE", 560, kF1Floor);
    sign("GAS", -362, kRoof);
    sign("GALLERY", 0, kRoof);
    sign("DEMOLITION", 362, kRoof);
    sign("FLUE", 657, kRoof);
    sign("CISTERN", -362, 0);
    // the big one on the roof
    concrete(-62, kRoof - 46, 62, kRoof - 22);
    beam(-50, kRoof - 21, -46, kRoof - 1);
    beam(46, kRoof - 21, 50, kRoof - 1);
    textCentred(id("sign_amber", "lamp_lit"), "SHOWCASE", 0, kRoof - 42, 3);

    // ---- lobby: spawn, stairs up to the gallery, a bench ----
    room("lobby", -194, kGF, 194, -1);
    // 27 steps of 4 x 4 px (the player steps up 4 px); the slab is open over the top
    // eight where the head would hit it, with a timber walkway over the lower part of
    // the opening (a 28 px gap to jump at the top; the top step meets the slab at x 64)
    air(27, kF1Floor, 63, kF1Floor + kSlab - 1);
    beam(27, kF1Floor, 35, kF1Floor + 2);
    for (int k = 1; k <= 27; ++k) concrete(-40 + 4 * (k - 1), -4 * k, -37 + 4 * (k - 1), std::min(-4 * k + 5, -1));
    s.spawnX_ = -160.5f;
    s.spawnY_ = 0.0f;

    // ---- gallery: shelves of material samples in glass jars ----
    room("gallery", -194, kF1, 194, kF1Floor - 1);
    beam(-182, -150, 40, -148);
    const char* liquids[] = {"water", "lamp_oil", "amber_syrup", "quickmetal", "tincture", "etchant", "brine"};
    const char* powders[] = {"sand", "cinder_grit", "rust_salt", "ash"};
    int jx = -178;
    auto jar = [&](const char* m) {
        rect(glass, jx, -162, jx, -151);
        rect(glass, jx + 11, -162, jx + 11, -151);
        rect(glass, jx, -151, jx + 11, -151);
        const MaterialId c = id(m, nullptr);
        if (c != kEmpty) rect(c, jx + 1, -159, jx + 10, -152);
        jx += 16;
    };
    for (const char* m : liquids) jar(m);
    jx += 8;
    for (const char* m : powders) jar(m);

    // ---- silo: layered powders over a hopper with a timber gate, a pressure plate under it ----
    room("silo", -690, kGF, -531, -1);
    {
        const int L = -670, R = -551, top = -312;
        concrete(L, top, R, top + 11);                   // cap
        sign("SILO", (L + R) / 2, top);
        concrete(L, top + 12, L + 7, -60);               // walls
        concrete(R - 7, top + 12, R, -60);
        const int in0 = L + 8, in1 = R - 8;              // -662 .. -559
        air(in0, top + 12, in1, -61);
        // hopper: the inside narrows from the full width at y -100 to a 10 px outlet at y -61
        for (int y = -100; y <= -61; ++y) {
            const float t = static_cast<float>(y + 100) / 39.0f;
            const int lx = in0 + static_cast<int>(t * 46.0f), rx = in1 - static_cast<int>(t * 46.0f);
            concrete(in0, y, lx, y);
            concrete(rx, y, in1, y);
        }
        concrete(in0, -60, -616, -55);
        concrete(-605, -60, in1, -55);
        beam(-615, -60, -606, -55);                      // the gate: dig or burn it
        // powders, densest at the bottom so nothing churns before the gate opens
        for (int y = top + 32; y <= -61; ++y)
            for (int x = in0; x <= in1; ++x) {
                if (s.mat_[static_cast<std::size_t>(y - s.y0_) * static_cast<std::size_t>(s.w_) + static_cast<std::size_t>(x - s.x0_)] != kEmpty) continue;
                const MaterialId m = y > -130 ? grit : (y > -175 ? id("rust_salt", "sand") : sand);
                put(x, y, m);
            }
        // lights hung under the hopper, clear of the outlet
        lampLine(-660, -622, -59, 1);
        lampLine(-600, -562, -59, 1);
        // pressure plate in the floor under the outlet: sand on it lights the floor lamp
        rect(id("volt_cell"), -688, 0, -683, 9);
        rect(copper, -682, 0, -626, 1);
        rect(id("press_plate"), -625, 0, -596, 1);
        rect(copper, -595, 0, -560, 1);
        rect(id("lamp"), -559, 0, -548, 4);
        rect(copper, -547, 0, -540, 1);
        rect(id("ground_rod"), -539, 0, -534, 9);
    }

    // ---- fluids: three glass tanks with timber bungs, a drain to the cistern ----
    room("fluids", -522, kGF, -203, -1);
    {
        const MaterialId fill[3] = {water, oil, brine};
        for (int i = 0; i < 3; ++i) {
            const int tx = -512 + 80 * i;
            concrete(tx, -17, tx + 70, -1);                 // plinth
            rect(glass, tx, -20, tx + 70, -18);              // floor
            rect(glass, tx, -75, tx + 2, -21);               // walls
            rect(glass, tx + 68, -75, tx + 70, -21);
            beam(tx + 68, -27, tx + 70, -21);                // the valve: a timber bung
            rect(fill[i], tx + 3, -70, tx + 67, -21);
        }
        // drain: a 5 px hole in the floor (the player is 7 wide), into the cistern below
        air(-235, 0, -231, 11);
        // a little pump on the floor: a basin of tincture to mix in
        rect(glass, -270, -14, -268, -1);
        rect(glass, -250, -14, -248, -1);
        rect(id("tincture", "water"), -267, -10, -251, -1);
    }
    // cistern under the fluids room
    room("cistern", -522, 12, -203, 109);
    concrete(-530, 12, -195, 121);
    air(-522, 12, -203, 109);
    girder(-522, -203, 12);
    air(-235, 12, -231, 15);                     // the drain goes through the girder
    lampLine(-518, -250, 12, 2);
    rect(water, -522, 100, -203, 109);

    // ---- electrical room ----
    room("power", 203, kGF, 522, -1);
    {
        const MaterialId volt = id("volt_cell"), ground = id("ground_rod"), sw = id("switch_open"), lamp = id("lamp");
        // 1. a lamp behind a switch, on a shelf just over head height
        concrete(208, -26, 298, -23);
        rect(volt, 212, -42, 217, -27);
        rect(copper, 218, -35, 240, -34);
        rect(sw, 241, -38, 246, -31);
        rect(copper, 247, -35, 262, -34);
        rect(lamp, 263, -44, 274, -27);
        rect(copper, 275, -35, 288, -34);
        rect(ground, 289, -42, 294, -27);
        // 2. a pressure plate flush with the floor: step on it and the floor light comes on
        rect(volt, 214, 0, 219, 9);
        rect(copper, 220, 0, 239, 1);
        rect(id("press_plate"), 240, 0, 259, 1);
        rect(copper, 260, 0, 279, 1);
        rect(lamp, 280, 0, 295, 4);
        rect(copper, 296, 0, 309, 1);
        rect(ground, 310, 0, 315, 9);
        // 3. a brine tank with copper electrodes, behind a switch: hydrogen and oxygen bubble off
        concrete(302, -27, 395, -24);
        rect(glass, 320, -30, 390, -28);
        rect(glass, 320, -70, 322, -31);
        rect(glass, 388, -70, 390, -31);
        rect(brine, 323, -62, 387, -31);
        rect(volt, 304, -44, 309, -28);
        rect(sw, 310, -40, 315, -33);
        rect(copper, 316, -80, 317, -35);
        rect(copper, 316, -80, 331, -79);
        rect(copper, 330, -78, 331, -40);
        rect(copper, 377, -80, 400, -79);
        rect(copper, 377, -78, 378, -40);
        rect(ground, 401, -84, 406, -71);
        // 4. a heater coil under a glass pot of water, behind a switch (thick leads stay cool)
        concrete(400, -30, 500, -27);
        rect(volt, 404, -50, 409, -31);
        rect(copper, 410, -42, 430, -40);
        rect(sw, 431, -45, 436, -37);
        rect(copper, 437, -42, 460, -40);
        rect(id("heater_coil"), 461, -42, 466, -40);
        rect(copper, 467, -42, 490, -40);
        rect(ground, 491, -50, 496, -31);
        rect(glass, 453, -45, 474, -43);
        rect(glass, 453, -70, 455, -46);
        rect(glass, 472, -70, 474, -46);
        rect(water, 456, -66, 471, -46);
    }

    // ---- furnace room: a firebrick furnace with logs, an igniter coil in the floor, a chimney ----
    room("furnace", 531, kGF, 690, -1);
    {
        rect(brick, 548, -66, 672, -1, H::Pinned, Panel);
        air(554, -60, 666, -1);
        air(548, -12, 553, -5);                         // air vents low in both walls
        air(667, -12, 672, -5);
        air(600, -66, 620, -61);                        // the flue opening
        beam(560, -7, 598, -1);                         // logs
        beam(606, -7, 655, -1);
        beam(572, -14, 640, -8);
        beam(590, -21, 622, -15);
        // igniter: volt cell -> switch (flush in the floor, by the door) -> thick leads -> a thin
        // fuse wire across the gap between the logs (it sparks and lights them) -> ground rod
        rect(id("volt_cell"), 533, 0, 538, 9);
        rect(copper, 539, 0, 540, 2);
        rect(id("switch_open"), 541, 0, 546, 2);
        rect(copper, 547, 0, 591, 2);
        rect(copper, 592, 0, 612, 0);
        rect(copper, 613, 0, 640, 2);
        rect(id("ground_rod"), 641, 0, 646, 9);
        // the chimney: firebrick up to the slab, glass through the flue room, a stack over the roof
        rect(brick, 596, -96, 599, -67, H::Pinned, Panel);
        rect(brick, 621, -96, 624, -67, H::Pinned, Panel);
        air(600, kF1Floor, 620, -97);
        rect(glass, 596, kF1, 599, kF1Floor - 1);
        rect(glass, 621, kF1, 624, kF1Floor - 1);
        air(600, kRoof, 620, kF1 - 1);
        rect(brick, 594, -296, 599, kRoof - 1, H::Pinned, Panel);
        rect(brick, 621, -296, 626, kRoof - 1, H::Pinned, Panel);
    }
    room("flue", 531, kF1, 690, kF1Floor - 1);

    // ---- gas room: a sealed chamber with hydrogen under its roof, an igniter and a blast door ----
    room("gas", -522, kF1, -203, kF1Floor - 1);
    {
        concrete(-480, -185, -303, -178);               // roof
        concrete(-480, -177, -473, kF1Floor - 1);       // walls
        concrete(-310, -177, -303, kF1Floor - 1);
        rect(iron, -310, -145, -303, -118, H::Loose);   // the blast door (loose: blast it or dig it)
        if (hydrogen != kEmpty)
            for (int y = -177; y <= -150; ++y)
                for (int x = -472; x <= -311; ++x)
                    if ((hash2(x, y) & 1) == 0) put(x, y, hydrogen);
        // igniter: volt cell -> switch outside the door -> a thin fuse wire into the chamber -> ground rod
        rect(id("volt_cell"), -287, -130, -282, -109);
        rect(copper, -292, -112, -288, -112);
        rect(id("switch_open"), -296, -115, -293, -109);
        rect(copper, -335, -112, -297, -112);
        rect(id("ground_rod"), -341, -130, -336, -109);
        beam(-334, -118, -318, -116);                    // kindling over the fuse...
        beam(-322, -165, -319, -119);                    // ...and a post up into the gas
    }

    // ---- demolition hall: a timber beam on wall corbels carrying loose blocks over a water tub,
    //      and a stack of slabs on timber props (cut them and it comes down) ----
    room("demolition", 203, kF1, 522, kF1Floor - 1);
    {
        concrete(203, -156, 214, -140);                  // a corbel on the wall...
        concrete(424, -156, 433, kF1Floor - 1);          // ...and a column
        beam(203, -162, 433, -157);
        circle(slate, 250, -171, 8, H::Loose);           // a boulder
        rect(id("concrete"), 290, -178, 307, -163, H::Loose, Formwork);
        rect(iron, 340, -174, 351, -163, H::Loose);      // an iron weight
        circle(id("basalt", "slate"), 400, -169, 6, H::Loose);
        // water tub on the floor under the beam
        rect(glass, 300, -111, 420, -109);
        rect(glass, 300, -122, 302, -112);
        rect(glass, 418, -122, 420, -112);
        rect(water, 303, -119, 417, -112);
        // the stack: three tiers of concrete slabs on timber props, all loose
        const MaterialId cm = id("concrete");
        for (int tier = 0; tier < 3; ++tier) {
            const int base = kF1Floor - 1 - tier * 26;   // -109, -135, -161
            beam(450, base - 19, 454, base, H::Loose);
            beam(494, base - 19, 498, base, H::Loose);
            rect(cm, 444, base - 25, 504, base - 20, H::Loose, Formwork);
        }
    }

    // ---- outside west: a cave down to the water table, trees, a sign ----
    room("cave", -1040, 0, -760, 230);
    {
        const int path[][2] = {{-880, -2}, {-892, 30}, {-866, 62}, {-904, 98}, {-910, 140}};
        tunnel(path, 5, 9);
        ellipse(kEmpty, -905, 175, 125, 42);
        for (int y = 186; y <= 217; ++y)
            for (int x = -1030; x <= -780; ++x) {
                const std::size_t i = static_cast<std::size_t>(y - s.y0_) * static_cast<std::size_t>(s.w_) + static_cast<std::size_t>(x - s.x0_);
                if (s.mat_[i] == kEmpty) put(x, y, water);    // the water table
            }
        circle(kEmpty, -1000, 150, 14);                       // a side pocket with lamp oil seeping in
        rect(oil, -1012, 152, -988, 164);
        // a work light under the cavern roof
        rect(id("volt_cell"), -930, 126, -925, 136);
        rect(id("lamp"), -924, 132, -909, 137);
        rect(id("ground_rod"), -908, 126, -903, 136);
        signBoard("CAVE", -820);
        tree(-760, 58, 20);
        tree(-980, 72, 24);
        tree(-1060, 50, 16);
    }

    // ---- outside east: the course (jet tower, grapple gantry over a raft pond, stepping pillars, a cliff) ----
    room("course", 701, -230, 1460, 60);
    {
        signBoard("COURSE", 750);
        // jet tower with a beacon on top and a passage through its base
        concrete(820, -200, 850, -1);
        air(820, -32, 850, -1);
        rect(id("volt_cell"), 821, -210, 826, -201);
        rect(id("lamp"), 827, -208, 842, -203);
        rect(id("ground_rod"), 843, -210, 848, -201);
        // the pond (dug into the ground) with two loose timber rafts
        air(880, 0, 1080, 11);
        concrete(880, 0, 885, 60);
        concrete(1075, 0, 1080, 60);
        concrete(880, 55, 1080, 60);
        rect(water, 886, 12, 1074, 54);
        air(886, 0, 1074, 11);
        beam(905, 6, 952, 11, H::Live);
        beam(1005, 6, 1052, 11, H::Live);
        // the gantry: a beam high over the pond to grapple from, on two posts with passages
        concrete(866, -190, 875, -1);
        air(866, -32, 875, -1);
        concrete(1085, -190, 1094, -1);
        air(1085, -32, 1094, -1);
        beam(866, -196, 1094, -191);
        lampLine(878, 1082, -194, 2);                    // work lights under it, over the pond
        lampPost(854);
        lampPost(1106);
        // stepping pillars and a slate cliff with a beacon
        const int heights[] = {20, 40, 60, 84};
        for (int k = 0; k < 4; ++k) concrete(1120 + 40 * k, -heights[k], 1136 + 40 * k, -1);
        rect(slate, 1290, -120, 1440, -1);
        for (int x = 1290; x <= 1440; ++x) {
            const int bump = static_cast<int>(mix(static_cast<std::uint32_t>(x / 5) * 0x165667B1u) % 5);
            air(x, -120, x, -120 + bump - 1);
            rect(turf, x, -120 + bump, x, -118 + bump);
        }
        beam(1430, -170, 1432, -121);
        rect(id("volt_cell"), 1424, -182, 1429, -173);
        rect(id("lamp"), 1430, -180, 1445, -175);
        rect(id("ground_rod"), 1446, -182, 1451, -173);
        concrete(1424, -172, 1451, -171);
        tree(1472, 66, 22);
    }
    (void)timber;
}

Showcase::Showcase(const MaterialRegistry& mats) : mats_(mats) {
    x0_ = kBoxX0;
    y0_ = kBoxY0;
    w_ = kBoxX1 - kBoxX0 + 1;
    h_ = kBoxY1 - kBoxY0 + 1;
    const std::size_t n = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
    mat_.assign(n, kGenerated);
    shade_.assign(n, 0);
    hold_.assign(n, Pinned);
    // air over the whole site first, so the generated hills don't poke into it
    for (int y = kBoxY0; y < 0; ++y)
        for (int x = kBoxX0; x <= kBoxX1; ++x) mat_[static_cast<std::size_t>(y - y0_) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x - x0_)] = kEmpty;
    ShowcasePainter p{*this, mats};
    p.paint();
}

const Showcase::Room* Showcase::room(const std::string& name) const {
    for (const Room& r : rooms_)
        if (r.name == name) return &r;
    return nullptr;
}

int Showcase::surface(int x, int generated) const {
    if (x >= kBoxX0 && x <= kBoxX1) return 0;
    const int d = x < kBoxX0 ? kBoxX0 - x : x - kBoxX1;
    if (d >= kBlend) return generated;
    float t = static_cast<float>(d) / static_cast<float>(kBlend);
    t = t * t * (3.0f - 2.0f * t);
    return static_cast<int>(std::lround(static_cast<float>(generated) * t));
}

bool Showcase::cellAt(int x, int y, Cell& out) const {
    if (x < x0_ || y < y0_ || x >= x0_ + w_ || y >= y0_ + h_) return false;
    const std::size_t i = static_cast<std::size_t>(y - y0_) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x - x0_);
    const std::uint16_t m = mat_[i];
    if (m == kGenerated) return false;
    if (m == kEmpty) { out = Cell{}; return true; }
    out = mats_[m].spawn(shade_[i] ? shade_[i] : terrainShade(x, y));
    if (mats_[m].kind == MaterialKind::Solid) out.aux = hold_[i] == Pinned ? kAuxPinned : (hold_[i] == Loose ? kAuxSettled : 0);
    return true;
}

} // namespace notita
