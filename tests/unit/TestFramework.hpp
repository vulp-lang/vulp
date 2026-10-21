#ifndef VULP_TEST_FRAMEWORK_HPP
#define VULP_TEST_FRAMEWORK_HPP

// A tiny dependency-free unit test framework.
//
//   TEST_CASE("lexer: numbers") { CHECK_EQ(1 + 1, 2); }
//
// Run all tests with `vulp_unit_tests`, or a subset with `vulp_unit_tests <substring>`.

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace vtest {
    struct TestCase {
        std::string name;
        std::function<void()> fn;
    };

    inline std::vector<TestCase>& registry() {
        static std::vector<TestCase> tests;
        return tests;
    }

    inline int& current_failures() {
        static int failures = 0;
        return failures;
    }

    inline void report(const char* file, const int line, const std::string& expr, const std::string& detail) {
        current_failures()++;
        std::cerr << "    FAILED " << file << ":" << line << ": " << expr << detail << "\n";
    }

    struct Registrar {
        Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
    };
} // namespace vtest

#define VT_CAT2(a, b) a##b
#define VT_CAT(a, b) VT_CAT2(a, b)
#define TEST_CASE(name)                                                                                    \
    static void VT_CAT(vt_test_fn_, __LINE__)();                                                           \
    static const vtest::Registrar VT_CAT(vt_test_reg_, __LINE__)(name, VT_CAT(vt_test_fn_, __LINE__));     \
    static void VT_CAT(vt_test_fn_, __LINE__)()

#define CHECK(cond)                                                                                        \
    do {                                                                                                   \
        if (!(cond)) vtest::report(__FILE__, __LINE__, #cond, "");                                         \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        const auto vt_a = (a);                                                                             \
        const auto vt_b = (b);                                                                             \
        if (!(vt_a == vt_b)) {                                                                             \
            std::ostringstream vt_os;                                                                      \
            vt_os << "\n      lhs: " << vt_a << "\n      rhs: " << vt_b;                                   \
            vtest::report(__FILE__, __LINE__, #a " == " #b, vt_os.str());                                  \
        }                                                                                                  \
    } while (0)

// CHECK that `haystack` (a std::string) contains `needle`.
#define CHECK_CONTAINS(haystack, needle)                                                                   \
    do {                                                                                                   \
        const std::string vt_h = (haystack);                                                               \
        const std::string vt_n = (needle);                                                                 \
        if (vt_h.find(vt_n) == std::string::npos) {                                                        \
            vtest::report(__FILE__, __LINE__, "CHECK_CONTAINS(" #haystack ", " #needle ")",                \
                          "\n      text:   " + vt_h + "\n      needle: " + vt_n);                          \
        }                                                                                                  \
    } while (0)

#endif // VULP_TEST_FRAMEWORK_HPP
