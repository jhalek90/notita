#pragma once
// Tiny self-contained test harness (no external framework to fetch).
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

} // namespace test

#define TEST_CASE(name)                                         \
    static void name();                                         \
    static ::test::Registrar name##_registrar(#name, &name);    \
    static void name()

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            ++::test::failures();                                                    \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        const auto va_ = (a);                                                        \
        const auto vb_ = (b);                                                        \
        if (!(va_ == vb_)) {                                                         \
            ++::test::failures();                                                    \
            std::printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, \
                        #a, #b, static_cast<long long>(va_), static_cast<long long>(vb_)); \
        }                                                                            \
    } while (0)
