#include "ChunkCodec.h"

#include "Rng.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace notita {

std::string ByteReader::str() {
    const std::uint32_t n = u32();
    if (!ok_ || static_cast<std::size_t>(end_ - p_) < n) { ok_ = false; return {}; }
    std::string s(reinterpret_cast<const char*>(p_), n);
    p_ += n;
    return s;
}

std::vector<std::uint8_t> ByteReader::blob() {
    const std::uint32_t n = u32();
    if (!ok_ || static_cast<std::size_t>(end_ - p_) < n) { ok_ = false; return {}; }
    std::vector<std::uint8_t> b(p_, p_ + n);
    p_ += n;
    return b;
}

void ByteReader::raw(void* out, std::size_t n) {
    if (!ok_ || static_cast<std::size_t>(end_ - p_) < n) {
        ok_ = false;
        std::memset(out, 0, n);
        return;
    }
    std::memcpy(out, p_, n);
    p_ += n;
}

// Control byte c: c < 128 -> c + 1 literal bytes follow; c >= 128 -> the next
// byte repeats c - 125 times (3..130). Runs of 2 stay literal.
void rleEncode(const std::uint8_t* data, std::size_t n, std::vector<std::uint8_t>& out) {
    std::size_t i = 0;
    while (i < n) {
        std::size_t run = 1;
        while (i + run < n && run < 130 && data[i + run] == data[i]) ++run;
        if (run >= 3) {
            out.push_back(static_cast<std::uint8_t>(run + 125));
            out.push_back(data[i]);
            i += run;
            continue;
        }
        // literal stretch until the next run of 3 (or 128 bytes)
        std::size_t j = i;
        while (j < n && j - i < 128) {
            if (j + 2 < n && data[j] == data[j + 1] && data[j] == data[j + 2]) break;
            ++j;
        }
        out.push_back(static_cast<std::uint8_t>(j - i - 1));
        out.insert(out.end(), data + i, data + j);
        i = j;
    }
}

bool rleDecode(ByteReader& in, std::uint8_t* data, std::size_t n) {
    std::size_t i = 0;
    while (i < n) {
        const std::uint8_t c = in.u8();
        if (!in.ok()) return false;
        if (c < 128) {
            const std::size_t len = static_cast<std::size_t>(c) + 1;
            if (i + len > n) return false;
            in.raw(data + i, len);
            i += len;
        } else {
            const std::size_t len = static_cast<std::size_t>(c) - 125;
            if (i + len > n) return false;
            const std::uint8_t v = in.u8();
            std::memset(data + i, v, len);
            i += len;
        }
        if (!in.ok()) return false;
    }
    return true;
}


namespace {

constexpr std::uint8_t kCellFormat = 1;        // shade relative to terrainShade
constexpr std::uint8_t kCellFormatTexture = 2; // #481: shade relative to the material texture

// the shade the generator would give this pixel (the reference the shade plane is stored against)
std::uint8_t refShade(const MaterialRegistry* mats, MaterialId m, int x, int y) {
    if (mats && m < mats->size()) return (*mats)[m].shadeAt(x, y);
    return terrainShade(x, y);
}
constexpr std::uint8_t kGasFormat = 2; // 2 (#480): + fuel and soot planes; 1 still reads

void plane(ByteWriter& w, const std::uint8_t* p, std::size_t n) {
    std::vector<std::uint8_t> enc;
    rleEncode(p, n, enc);
    w.blob(enc);
}

bool readPlane(ByteReader& r, std::uint8_t* p, std::size_t n) {
    const std::vector<std::uint8_t> enc = r.blob();
    if (!r.ok()) return false;
    ByteReader pr(enc.data(), enc.size());
    return rleDecode(pr, p, n) && pr.atEnd();
}

// splits n 4-byte values into 4 byte planes (byte k of every value together)
template <class T>
void planes4(ByteWriter& w, const T* v, std::size_t n) {
    static_assert(sizeof(T) == 4);
    std::vector<std::uint8_t> b(n);
    for (int k = 0; k < 4; ++k) {
        for (std::size_t i = 0; i < n; ++i) {
            std::uint32_t u;
            std::memcpy(&u, &v[i], 4);
            b[i] = static_cast<std::uint8_t>(u >> (8 * k));
        }
        plane(w, b.data(), n);
    }
}

template <class T>
bool readPlanes4(ByteReader& r, T* v, std::size_t n) {
    static_assert(sizeof(T) == 4);
    std::vector<std::uint32_t> u(n, 0);
    std::vector<std::uint8_t> b(n);
    for (int k = 0; k < 4; ++k) {
        if (!readPlane(r, b.data(), n)) return false;
        for (std::size_t i = 0; i < n; ++i) u[i] |= static_cast<std::uint32_t>(b[i]) << (8 * k);
    }
    for (std::size_t i = 0; i < n; ++i) std::memcpy(&v[i], &u[i], 4);
    return true;
}

} // namespace

std::vector<std::uint8_t> encodeCells(const Chunk& chunk, const MaterialRegistry* mats) {
    constexpr std::size_t N = kChunkCells;
    ByteWriter w;
    w.u8(mats ? kCellFormatTexture : kCellFormat);
    // palette in order of first appearance (deterministic)
    std::vector<MaterialId> palette;
    std::array<std::uint8_t, N> idx{};
    bool wide = false;
    {
        std::vector<int> slot(65536, -1);
        for (std::size_t i = 0; i < N; ++i) {
            const MaterialId m = chunk.cells[i].material;
            if (slot[m] < 0) {
                slot[m] = static_cast<int>(palette.size());
                palette.push_back(m);
            }
            idx[i] = static_cast<std::uint8_t>(slot[m] & 0xFF);
        }
        wide = palette.size() > 256;
    }
    w.u8(wide ? 1 : 0);
    std::array<std::uint8_t, N> b{};
    if (!wide) {
        w.u16(static_cast<std::uint16_t>(palette.size()));
        for (MaterialId m : palette) w.u16(m);
        plane(w, idx.data(), N);
    } else {
        for (int k = 0; k < 2; ++k) {
            for (std::size_t i = 0; i < N; ++i) b[i] = static_cast<std::uint8_t>(chunk.cells[i].material >> (8 * k));
            plane(w, b.data(), N);
        }
    }
    const int ox = chunk.originX(), oy = chunk.originY();
    for (std::size_t i = 0; i < N; ++i) {
        const int x = ox + static_cast<int>(i & kChunkMask), y = oy + static_cast<int>(i >> kChunkShift);
        b[i] = static_cast<std::uint8_t>(chunk.cells[i].shade - refShade(mats, chunk.cells[i].material, x, y));
    }
    plane(w, b.data(), N);
    {
        std::array<float, N> t{};
        for (std::size_t i = 0; i < N; ++i) t[i] = chunk.cells[i].temperature;
        planes4(w, t.data(), N);
    }
    auto bytePlane = [&](auto get) {
        for (std::size_t i = 0; i < N; ++i) b[i] = get(chunk.cells[i]);
        plane(w, b.data(), N);
    };
    bytePlane([](const Cell& c) { return static_cast<std::uint8_t>(static_cast<std::uint16_t>(c.vx)); });
    bytePlane([](const Cell& c) { return static_cast<std::uint8_t>(static_cast<std::uint16_t>(c.vx) >> 8); });
    bytePlane([](const Cell& c) { return static_cast<std::uint8_t>(static_cast<std::uint16_t>(c.vy)); });
    bytePlane([](const Cell& c) { return static_cast<std::uint8_t>(static_cast<std::uint16_t>(c.vy) >> 8); });
    bytePlane([](const Cell& c) { return c.flags; });
    bytePlane([](const Cell& c) { return c.life; });
    bytePlane([](const Cell& c) { return c.aux; });
    bytePlane([](const Cell& c) { return static_cast<std::uint8_t>(c.stamp); });
    bytePlane([](const Cell& c) { return static_cast<std::uint8_t>(c.stamp >> 8); });
    return std::move(w.buf);
}

bool decodeCells(const std::vector<std::uint8_t>& blob, Chunk& chunk, const std::vector<MaterialId>* remap,
                 const MaterialRegistry* mats) {
    constexpr std::size_t N = kChunkCells;
    ByteReader r(blob.data(), blob.size());
    const std::uint8_t format = r.u8();
    if (format != kCellFormat && format != kCellFormatTexture) return false;
    const MaterialRegistry* ref = format == kCellFormatTexture ? mats : nullptr;
    const bool wide = r.u8() != 0;
    std::array<std::uint8_t, N> b{}, b2{};
    std::array<MaterialId, N> mat{};
    if (!wide) {
        const std::uint16_t n = r.u16();
        if (n == 0 || n > 256) return false;
        std::vector<MaterialId> palette(n);
        for (auto& m : palette) m = r.u16();
        if (!readPlane(r, b.data(), N)) return false;
        for (std::size_t i = 0; i < N; ++i) {
            if (b[i] >= n) return false;
            mat[i] = palette[b[i]];
        }
    } else {
        if (!readPlane(r, b.data(), N) || !readPlane(r, b2.data(), N)) return false;
        for (std::size_t i = 0; i < N; ++i) mat[i] = static_cast<MaterialId>(b[i] | (b2[i] << 8));
    }
    if (remap)
        for (auto& m : mat) m = m < remap->size() ? (*remap)[m] : kEmpty;
    for (std::size_t i = 0; i < N; ++i) chunk.cells[i].material = mat[i];
    if (!readPlane(r, b.data(), N)) return false;
    const int ox = chunk.originX(), oy = chunk.originY();
    for (std::size_t i = 0; i < N; ++i) {
        const int x = ox + static_cast<int>(i & kChunkMask), y = oy + static_cast<int>(i >> kChunkShift);
        chunk.cells[i].shade = static_cast<std::uint8_t>(b[i] + refShade(ref, chunk.cells[i].material, x, y));
    }
    {
        std::array<float, N> t{};
        if (!readPlanes4(r, t.data(), N)) return false;
        for (std::size_t i = 0; i < N; ++i) chunk.cells[i].temperature = t[i];
    }
    auto pair16 = [&](auto set) {
        if (!readPlane(r, b.data(), N) || !readPlane(r, b2.data(), N)) return false;
        for (std::size_t i = 0; i < N; ++i) set(chunk.cells[i], static_cast<std::uint16_t>(b[i] | (b2[i] << 8)));
        return true;
    };
    auto byte = [&](auto set) {
        if (!readPlane(r, b.data(), N)) return false;
        for (std::size_t i = 0; i < N; ++i) set(chunk.cells[i], b[i]);
        return true;
    };
    if (!pair16([](Cell& c, std::uint16_t v) { c.vx = static_cast<std::int16_t>(v); })) return false;
    if (!pair16([](Cell& c, std::uint16_t v) { c.vy = static_cast<std::int16_t>(v); })) return false;
    if (!byte([](Cell& c, std::uint8_t v) { c.flags = v; })) return false;
    if (!byte([](Cell& c, std::uint8_t v) { c.life = v; })) return false;
    if (!byte([](Cell& c, std::uint8_t v) { c.aux = v; })) return false;
    if (!pair16([](Cell& c, std::uint16_t v) { c.stamp = v; })) return false;
    return r.ok() && r.atEnd();
}

std::vector<std::uint8_t> encodeGas(const GasGrid::ChunkCells& g) {
    constexpr std::size_t S = GasGrid::kChunkCellsSide * GasGrid::kChunkCellsSide;
    ByteWriter w;
    w.u8(kGasFormat);
    w.u8(static_cast<std::uint8_t>(g.species));
    if (g.empty()) { w.u8(0); return std::move(w.buf); }
    w.u8(1);
    for (int k = 0; k < g.species; ++k) planes4(w, g.mass.data() + static_cast<std::size_t>(k) * S, S);
    planes4(w, g.heat.data(), S);
    planes4(w, g.u.data(), S);
    planes4(w, g.v.data(), S);
    planes4(w, g.p.data(), S);
    planes4(w, g.extra.data(), S);
    planes4(w, g.open.data(), S);
    planes4(w, g.src.data(), S);
    plane(w, g.burnt.data(), S);
    std::vector<float> zero;
    if (g.fuel.size() < S || g.soot.size() < S) zero.assign(S, 0.0f);
    planes4(w, g.fuel.size() >= S ? g.fuel.data() : zero.data(), S);
    planes4(w, g.soot.size() >= S ? g.soot.data() : zero.data(), S);
    return std::move(w.buf);
}

bool decodeGas(const std::vector<std::uint8_t>& blob, GasGrid::ChunkCells& g, int currentSpecies,
               const std::vector<int>* speciesRemap) {
    constexpr std::size_t S = GasGrid::kChunkCellsSide * GasGrid::kChunkCellsSide;
    ByteReader r(blob.data(), blob.size());
    const std::uint8_t format = r.u8();
    if (format != 1 && format != kGasFormat) return false;
    const int ns = r.u8();
    g = GasGrid::ChunkCells{};
    if (r.u8() == 0) return r.ok();
    g.species = currentSpecies;
    g.mass.assign(static_cast<std::size_t>(currentSpecies) * S, 0);
    std::vector<std::int32_t> m(S);
    for (int k = 0; k < ns; ++k) {
        if (!readPlanes4(r, m.data(), S)) return false;
        const int to = speciesRemap ? (k < static_cast<int>(speciesRemap->size()) ? (*speciesRemap)[k] : -1) : k;
        if (to < 0 || to >= currentSpecies) continue; // gas the current data doesn't have
        for (std::size_t i = 0; i < S; ++i) g.mass[static_cast<std::size_t>(to) * S + i] += m[i];
    }
    g.heat.resize(S); g.u.resize(S); g.v.resize(S); g.p.resize(S); g.extra.resize(S); g.open.resize(S); g.src.resize(S); g.burnt.resize(S);
    if (!readPlanes4(r, g.heat.data(), S) || !readPlanes4(r, g.u.data(), S) || !readPlanes4(r, g.v.data(), S) ||
        !readPlanes4(r, g.p.data(), S) || !readPlanes4(r, g.extra.data(), S) || !readPlanes4(r, g.open.data(), S) ||
        !readPlanes4(r, g.src.data(), S) || !readPlane(r, g.burnt.data(), S))
        return false;
    if (format >= 2) {
        g.fuel.resize(S);
        g.soot.resize(S);
        if (!readPlanes4(r, g.fuel.data(), S) || !readPlanes4(r, g.soot.data(), S)) return false;
    }
    return r.ok() && r.atEnd();
}

} // namespace notita
