// Minimal test harness: registration-based, exact pass/fail counts, no deps.
#pragma once
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace harness {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int g_checks = 0;
inline int g_failures = 0;
inline const char* g_current = "";

inline void report_fail(const char* expr, const char* file, int line, const std::string& detail = "") {
    ++g_failures;
    std::printf("FAIL [%s] %s at %s:%d%s%s\n", g_current, expr, file, line,
                detail.empty() ? "" : " -- ", detail.c_str());
}

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    ++g_checks;
    if (!(a == b)) {
        std::ostringstream os;
        os << ea << "=" << a << ", " << eb << "=" << b;
        report_fail("CHECK_EQ", file, line, os.str());
    }
}

inline int run_all() {
    for (const auto& t : registry()) {
        g_current = t.name;
        t.fn();
    }
    std::printf("\n%zu tests, %d checks, %d failures\n", registry().size(), g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace harness

#define TEST(name)                                              \
    static void test_##name();                                  \
    static harness::Registrar reg_##name(#name, &test_##name);  \
    static void test_##name()

#define CHECK(cond)                                             \
    do {                                                        \
        ++harness::g_checks;                                    \
        if (!(cond)) harness::report_fail(#cond, __FILE__, __LINE__); \
    } while (0)

#define CHECK_EQ(a, b) harness::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
