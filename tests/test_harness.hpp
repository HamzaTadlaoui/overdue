#pragma once
// A tiny, dependency-free test harness. Each test file registers cases with
// TEST(name){ ... }; a single test_main.cpp calls th::run_all(). CHECK macros
// record a failure and keep going, so one bad assertion doesn't hide the rest.
// A substring filter (argv[1]) lets CTest register several add_test() entries
// against the one binary for granular reporting.

#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace th {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int& current_failures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        registry().push_back({std::move(name), std::move(fn)});
    }
};

inline void report_fail(const char* file, int line, const std::string& msg) {
    std::printf("    FAIL %s:%d: %s\n", file, line, msg.c_str());
    ++current_failures();
}

inline int run_all(std::string_view filter = "") {
    int passed = 0, failed = 0;
    for (auto& tc : registry()) {
        if (!filter.empty() && tc.name.find(filter) == std::string::npos) continue;
        int before = current_failures();
        try {
            tc.fn();
        } catch (const std::exception& e) {
            report_fail(tc.name.c_str(), 0, std::string("uncaught exception: ") + e.what());
        } catch (...) {
            report_fail(tc.name.c_str(), 0, "uncaught unknown exception");
        }
        if (current_failures() == before) { std::printf("[PASS] %s\n", tc.name.c_str()); ++passed; }
        else { std::printf("[FAIL] %s\n", tc.name.c_str()); ++failed; }
    }
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

} // namespace th

#define TEST(name)                                                    \
    static void name();                                               \
    static th::Registrar registrar_##name(#name, name);              \
    static void name()

#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) th::report_fail(__FILE__, __LINE__, "CHECK failed: " #cond); \
    } while (0)

#define CHECK_MSG(cond, msg)                                          \
    do {                                                              \
        if (!(cond))                                                  \
            th::report_fail(__FILE__, __LINE__,                       \
                            std::string("CHECK failed: " #cond " — ") + (msg)); \
    } while (0)

// Compares two values with ==; on failure prints both for diagnosis.
#define CHECK_EQ(a, b)                                                \
    do {                                                              \
        auto _a = (a);                                                \
        auto _b = (b);                                                \
        if (!(_a == _b))                                              \
            th::report_fail(__FILE__, __LINE__,                       \
                std::string("CHECK_EQ failed: " #a " == " #b));       \
    } while (0)

#define CHECK_THROWS(expr, ExcType)                                   \
    do {                                                              \
        bool _threw = false;                                          \
        try { (void)(expr); }                                         \
        catch (const ExcType&) { _threw = true; }                     \
        catch (...) {}                                                \
        if (!_threw)                                                  \
            th::report_fail(__FILE__, __LINE__,                       \
                "expected " #ExcType " from " #expr);                 \
    } while (0)
