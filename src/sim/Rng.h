#pragma once
// Small deterministic RNG. Every chunk update seeds its own stream from
// (world seed, tick, chunk coords, pass), so results never depend on which
// thread ran which chunk or in what order.
#include <cstdint>

namespace notita {

constexpr std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

class Rng {
public:
    Rng() = default;
    explicit Rng(std::uint64_t seed) { reseed(seed); }

    void reseed(std::uint64_t seed) {
        s0_ = splitmix64(seed);
        s1_ = splitmix64(s0_ ^ 0xD1B54A32D192ED03ull);
        if ((s0_ | s1_) == 0) s1_ = 1;
    }

    // xoroshiro128+ (fast, good enough for a cellular automaton)
    std::uint64_t next() {
        const std::uint64_t a = s0_;
        std::uint64_t b = s1_;
        const std::uint64_t r = a + b;
        b ^= a;
        s0_ = rotl(a, 24) ^ b ^ (b << 16);
        s1_ = rotl(b, 37);
        return r;
    }
    std::uint32_t u32() { return static_cast<std::uint32_t>(next() >> 32); }
    // [0, n)
    std::uint32_t below(std::uint32_t n) {
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(u32()) * n) >> 32);
    }
    bool coin() { return (next() >> 63) != 0; }
    // probability p in [0,1], expressed as 16-bit threshold to stay integer-exact
    bool chance16(std::uint32_t threshold65536) { return (u32() >> 16) < threshold65536; }
    float unit() { return static_cast<float>(u32() >> 8) * (1.0f / 16777216.0f); }

    static std::uint64_t hash(std::uint64_t a, std::uint64_t b) {
        return splitmix64(a ^ splitmix64(b + 0x632BE59BD9B4E019ull));
    }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    std::uint64_t s0_ = 1, s1_ = 2;
};

} // namespace notita
