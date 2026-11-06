#pragma once
// Phase 6: compact encoding of a chunk's state, used both for sleeping chunks
// kept in memory and for save files.
//
// Cells are split into byte planes (material palette index, shade, the four
// bytes of the temperature, velocity, flags, life, aux, stamp) and every plane
// is run-length encoded (PackBits style). A palette maps the chunk's few
// materials to one byte. Shade is stored as the difference to the shade the
// world generator gives that pixel, so untouched terrain is all zeros. A rock
// chunk at ambient temperature shrinks from 64 KiB to a few hundred bytes.
//
// The gas grid cells over the chunk (GasGrid::ChunkCells) are encoded the
// same way (int32/float values split into byte planes, each RLE'd).
#include "Chunk.h"
#include "GasGrid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace notita {

// Little-endian byte writer / reader for the save format.
class ByteWriter {
public:
    std::vector<std::uint8_t> buf;
    void u8(std::uint8_t v) { buf.push_back(v); }
    void u16(std::uint16_t v) { raw(&v, 2); }
    void u32(std::uint32_t v) { raw(&v, 4); }
    void u64(std::uint64_t v) { raw(&v, 8); }
    void i32(std::int32_t v) { raw(&v, 4); }
    void i64(std::int64_t v) { raw(&v, 8); }
    void f32(float v) { raw(&v, 4); }
    void f64(double v) { raw(&v, 8); }
    void str(const std::string& s) { u32(static_cast<std::uint32_t>(s.size())); raw(s.data(), s.size()); }
    void blob(const std::vector<std::uint8_t>& b) { u32(static_cast<std::uint32_t>(b.size())); raw(b.data(), b.size()); }
    void raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    }
};

class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) : p_(data), end_(data + size) {}
    bool ok() const { return ok_; }
    bool atEnd() const { return p_ == end_; }
    std::uint8_t u8() { std::uint8_t v = 0; raw(&v, 1); return v; }
    std::uint16_t u16() { std::uint16_t v = 0; raw(&v, 2); return v; }
    std::uint32_t u32() { std::uint32_t v = 0; raw(&v, 4); return v; }
    std::uint64_t u64() { std::uint64_t v = 0; raw(&v, 8); return v; }
    std::int32_t i32() { std::int32_t v = 0; raw(&v, 4); return v; }
    std::int64_t i64() { std::int64_t v = 0; raw(&v, 8); return v; }
    float f32() { float v = 0; raw(&v, 4); return v; }
    double f64() { double v = 0; raw(&v, 8); return v; }
    std::string str();
    std::vector<std::uint8_t> blob();
    void raw(void* out, std::size_t n);

private:
    const std::uint8_t* p_;
    const std::uint8_t* end_;
    bool ok_ = true;
};

// PackBits-style run-length coding of a byte plane.
void rleEncode(const std::uint8_t* data, std::size_t n, std::vector<std::uint8_t>& out);
bool rleDecode(ByteReader& in, std::uint8_t* data, std::size_t n);

// Shade the generator gives a pixel (deterministic per position).
std::uint8_t terrainShade(int x, int y);

// remap: material id in the blob -> current id (null = identity).
std::vector<std::uint8_t> encodeCells(const Chunk& chunk);
bool decodeCells(const std::vector<std::uint8_t>& blob, Chunk& chunk, const std::vector<MaterialId>* remap = nullptr);

// speciesRemap: species index in the blob -> current index (-1 = dropped; null = identity).
std::vector<std::uint8_t> encodeGas(const GasGrid::ChunkCells& cells);
bool decodeGas(const std::vector<std::uint8_t>& blob, GasGrid::ChunkCells& cells, int currentSpecies,
               const std::vector<int>* speciesRemap = nullptr);

} // namespace notita
