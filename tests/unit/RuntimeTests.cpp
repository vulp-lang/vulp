#include "TestFramework.hpp"
#include "vulp_runtime.h"

#include <cstring>

__extension__ typedef __int128 vulp_i128;
__extension__ typedef unsigned __int128 vulp_u128;

namespace {
    template<class F>
    std::string capture(F&& fn) {
        vulp_rt_test_capture_begin();
        fn();
        return vulp_rt_test_capture_end();
    }

    std::string str(const VulpVec& v) {
        return v.len ? std::string(v.data, static_cast<size_t>(v.len)) : std::string();
    }

    VulpVec make_str(const std::string& s) {
        VulpVec v{};
        vulp_str_from_bytes(&v, s.data(), static_cast<int64_t>(s.size()));
        return v;
    }

    const char* I128_MAX = "170141183460469231731687303715884105727";
    const char* I128_MIN = "-170141183460469231731687303715884105728";
    const char* U128_MAX = "340282366920938463463374607431768211455";
    const char* I256_MAX = "57896044618658097711785492504343953926634992332820282019728792003956564819967";
    const char* I256_MIN = "-57896044618658097711785492504343953926634992332820282019728792003956564819968";
    const char* U256_MAX = "115792089237316195423570985008687907853269984665640564039457584007913129639935";
} // namespace

// ---- 4.1 output ----

TEST_CASE("runtime: print 64-bit integers") {
    CHECK_EQ(capture([] {
        vulp_print_i64(0);
        vulp_print_char(' ');
        vulp_print_i64(-42);
        vulp_print_char(' ');
        vulp_print_i64(INT64_MIN);
        vulp_print_char(' ');
        vulp_print_i64(INT64_MAX);
        vulp_print_char(' ');
        vulp_print_u64(UINT64_MAX);
    }), std::string("0 -42 -9223372036854775808 9223372036854775807 18446744073709551615"));
}

TEST_CASE("runtime: print 128-bit integers") {
    vulp_i128 max = static_cast<vulp_i128>((static_cast<vulp_u128>(1) << 127) - 1);
    vulp_i128 min = -max - 1;
    vulp_u128 umax = ~static_cast<vulp_u128>(0);
    CHECK_EQ(capture([&] { vulp_print_i128(&max); }), std::string(I128_MAX));
    CHECK_EQ(capture([&] { vulp_print_i128(&min); }), std::string(I128_MIN));
    CHECK_EQ(capture([&] { vulp_print_u128(&umax); }), std::string(U128_MAX));
}

TEST_CASE("runtime: print 256-bit integers") {
    uint64_t max[4] = {~0ULL, ~0ULL, ~0ULL, ~0ULL >> 1};
    uint64_t min[4] = {0, 0, 0, 1ULL << 63};
    uint64_t umax[4] = {~0ULL, ~0ULL, ~0ULL, ~0ULL};
    uint64_t minus_one[4] = {~0ULL, ~0ULL, ~0ULL, ~0ULL};
    uint64_t zero[4] = {0, 0, 0, 0};
    CHECK_EQ(capture([&] { vulp_print_i256(max); }), std::string(I256_MAX));
    CHECK_EQ(capture([&] { vulp_print_i256(min); }), std::string(I256_MIN));
    CHECK_EQ(capture([&] { vulp_print_u256(umax); }), std::string(U256_MAX));
    CHECK_EQ(capture([&] { vulp_print_i256(minus_one); }), std::string("-1"));
    CHECK_EQ(capture([&] { vulp_print_i256(zero); }), std::string("0"));
}

TEST_CASE("runtime: floats print in shortest round-trip form") {
    CHECK_EQ(capture([] { vulp_print_f64(0.1); }), std::string("0.1"));
    CHECK_EQ(capture([] { vulp_print_f64(1.0); }), std::string("1"));
    CHECK_EQ(capture([] { vulp_print_f64(-2.5); }), std::string("-2.5"));
    CHECK_EQ(capture([] { vulp_print_f64(1.0 / 3.0); }), std::string("0.3333333333333333"));
    CHECK_EQ(capture([] { vulp_print_f64(1e20); }), std::string("1e+20"));
    CHECK_EQ(capture([] { vulp_print_f64(3.14159); }), std::string("3.14159"));
}

TEST_CASE("runtime: floats use plain notation for moderate exponents (like Python's repr)") {
    const auto f = [](const double v) { return capture([v] { vulp_print_f64(v); }); };
    CHECK_EQ(f(100.0), std::string("100"));
    CHECK_EQ(f(1500.0), std::string("1500"));
    CHECK_EQ(f(100000.0), std::string("100000"));
    CHECK_EQ(f(100000.25), std::string("100000.25"));
    CHECK_EQ(f(-1500.0), std::string("-1500"));
    CHECK_EQ(f(700.0), std::string("700"));
    CHECK_EQ(f(1e15), std::string("1000000000000000"));
    CHECK_EQ(f(1e16), std::string("1e+16"));
    CHECK_EQ(f(123456789.125), std::string("123456789.125"));
    CHECK_EQ(f(0.0001), std::string("0.0001"));
    CHECK_EQ(f(0.00001), std::string("1e-05"));
    CHECK_EQ(f(1.5e-7), std::string("1.5e-07"));
    CHECK_EQ(f(2.5e-3), std::string("0.0025"));
    CHECK_EQ(f(0.0), std::string("0"));
    CHECK_EQ(f(-0.0), std::string("-0"));
    CHECK_EQ(f(1e300), std::string("1e+300"));
    CHECK_EQ(f(5e-324), std::string("5e-324"));
    CHECK_EQ(f(1.7976931348623157e308), std::string("1.7976931348623157e+308"));
    CHECK_EQ(f(1.0 / 0.0), std::string("inf"));
    CHECK_EQ(f(-1.0 / 0.0), std::string("-inf"));
}

TEST_CASE("runtime: chars, bools, byte strings") {
    CHECK_EQ(capture([] {
        vulp_print_char('x');
        vulp_print_bool(1);
        vulp_print_bool(0);
        vulp_print_bytes("abc", 3);
        const VulpVec s = make_str("-str");
        vulp_print_str(&s);
        const VulpVec empty{};
        vulp_print_str(&empty);
    }), std::string("xtruefalseabc-str"));
}

TEST_CASE("runtime: output larger than the buffer is not lost") {
    const std::string out = capture([] {
        for (int i = 0; i < 100000; i++) vulp_print_bytes("0123456789", 10);
    });
    CHECK_EQ(out.size(), 1000000u);
    CHECK_EQ(out.substr(999990), std::string("0123456789"));
}

// ---- 4.2 input ----

TEST_CASE("runtime: read integers of every width") {
    vulp_rt_test_set_input(std::string("  -42\n18446744073709551615 ") + I128_MAX + " " + I128_MIN + "\t" + U128_MAX
                           + "\n" + I256_MAX + " " + I256_MIN + " " + U256_MAX + " +7");
    CHECK_EQ(vulp_read_i64(1), -42);
    CHECK(vulp_read_u64(1) == UINT64_MAX);
    vulp_i128 a, b;
    vulp_u128 c;
    vulp_read_i128(&a, 1);
    vulp_read_i128(&b, 1);
    vulp_read_u128(&c, 1);
    CHECK_EQ(capture([&] { vulp_print_i128(&a); }), std::string(I128_MAX));
    CHECK_EQ(capture([&] { vulp_print_i128(&b); }), std::string(I128_MIN));
    CHECK_EQ(capture([&] { vulp_print_u128(&c); }), std::string(U128_MAX));
    uint64_t x[4], y[4], z[4];
    vulp_read_i256(x, 1);
    vulp_read_i256(y, 1);
    vulp_read_u256(z, 1);
    CHECK_EQ(capture([&] { vulp_print_i256(x); }), std::string(I256_MAX));
    CHECK_EQ(capture([&] { vulp_print_i256(y); }), std::string(I256_MIN));
    CHECK_EQ(capture([&] { vulp_print_u256(z); }), std::string(U256_MAX));
    CHECK_EQ(vulp_read_i64(1), 7);
}

TEST_CASE("runtime: read floats, chars and string tokens") {
    vulp_rt_test_set_input("3.25 -1e3\n  x yz hello world\n");
    CHECK(vulp_read_f64(1) == 3.25);
    CHECK(vulp_read_f64(1) == -1000.0);
    CHECK_EQ(vulp_read_char(1), 'x');
    CHECK_EQ(vulp_read_char(1), 'y');
    CHECK_EQ(vulp_read_char(1), 'z');
    VulpVec s{};
    vulp_read_str(&s, 1);
    CHECK_EQ(str(s), std::string("hello"));
    vulp_read_str(&s, 1); // reuses the buffer
    CHECK_EQ(str(s), std::string("world"));
}

TEST_CASE("runtime: reading many integers across buffer refills") {
    std::string input;
    for (int i = 0; i < 50000; i++) input += std::to_string(i * 7 - 1000) + (i % 10 == 9 ? "\n" : " ");
    vulp_rt_test_set_input(input);
    long long sum = 0, expect = 0;
    for (int i = 0; i < 50000; i++) {
        sum += vulp_read_i64(1);
        expect += i * 7 - 1000;
    }
    CHECK_EQ(sum, expect);
}

TEST_CASE("runtime: checked integer reads accept the full range of the target type") {
    vulp_rt_test_set_input("-128 127 255 0 -9223372036854775808 18446744073709551615");
    CHECK_EQ(vulp_read_int(8, 1, 1), -128);
    CHECK_EQ(vulp_read_int(8, 1, 1), 127);
    CHECK_EQ(vulp_read_int(8, 0, 1), 255);
    CHECK_EQ(vulp_read_int(1, 0, 1), 0);
    CHECK(vulp_read_i64(1) == INT64_MIN);
    CHECK(vulp_read_u64(1) == UINT64_MAX);
}

// ---- 4.3 sequences ----

TEST_CASE("runtime: vec alloc / push / clone") {
    VulpVec v{};
    vulp_vec_alloc(&v, 3, 8, 1);
    CHECK_EQ(v.len, 3);
    CHECK_EQ(reinterpret_cast<int64_t*>(v.data)[2], 0);
    for (int i = 0; i < 100; i++) {
        auto* slot = static_cast<int64_t*>(vulp_vec_push_slot(&v, 8));
        CHECK_EQ(*slot, 0);
        *slot = i;
    }
    CHECK_EQ(v.len, 103);
    CHECK_EQ(reinterpret_cast<int64_t*>(v.data)[102], 99);
    VulpVec w{};
    vulp_vec_clone(&w, &v, 8);
    CHECK(w.data != v.data);
    CHECK_EQ(w.len, 103);
    reinterpret_cast<int64_t*>(w.data)[5] = 77;
    CHECK_EQ(reinterpret_cast<int64_t*>(v.data)[5], 2);
    VulpVec empty{};
    vulp_vec_alloc(&empty, 0, 8, 1);
    CHECK(empty.data == nullptr);
    CHECK_EQ(empty.len, 0);
}

TEST_CASE("runtime: strings concat / append / compare / from int") {
    VulpVec a = make_str("abc"), b = make_str("de"), c{};
    vulp_str_concat(&c, &a, &b);
    CHECK_EQ(str(c), std::string("abcde"));
    vulp_str_append(&a, &b);
    CHECK_EQ(str(a), std::string("abcde"));
    vulp_str_append(&a, &a); // self-append
    CHECK_EQ(str(a), std::string("abcdeabcde"));
    VulpVec x = make_str("abc"), y = make_str("abd"), z = make_str("ab"), e{};
    CHECK(vulp_str_cmp(&x, &y) < 0);
    CHECK(vulp_str_cmp(&y, &x) > 0);
    CHECK(vulp_str_cmp(&z, &x) < 0);
    CHECK_EQ(vulp_str_cmp(&x, &x), 0);
    CHECK(vulp_str_cmp(&e, &z) < 0);
    VulpVec n{};
    vulp_str_from_i64(&n, -1234);
    CHECK_EQ(str(n), std::string("-1234"));
    vulp_str_from_u64(&n, UINT64_MAX);
    CHECK_EQ(str(n), std::string("18446744073709551615"));
}

TEST_CASE("runtime: string conversion of wide integers and floats") {
    VulpVec s{};
    vulp_i128 big = -(static_cast<vulp_i128>(1) << 100);
    vulp_str_from_i128(&s, &big);
    CHECK_EQ(str(s), std::string("-1267650600228229401496703205376"));
    uint64_t umax[4] = {~0ULL, ~0ULL, ~0ULL, ~0ULL};
    vulp_str_from_u256(&s, umax);
    CHECK_EQ(str(s), std::string(U256_MAX));
    vulp_str_from_i256(&s, umax); // all ones = -1
    CHECK_EQ(str(s), std::string("-1"));
    vulp_str_from_f64(&s, 2.5);
    CHECK_EQ(str(s), std::string("2.5"));
    vulp_str_from_f64(&s, 1e21);
    CHECK_EQ(str(s), std::string("1e+21"));
}

TEST_CASE("runtime: sort for every element kind") {
    VulpVec v{};
    vulp_vec_alloc(&v, 5, 4, 1);
    const int32_t vals[5] = {5, -1, 3, 0, -7};
    std::memcpy(v.data, vals, sizeof vals);
    vulp_sort(&v, VULP_SORT_I32);
    const auto* s = reinterpret_cast<int32_t*>(v.data);
    CHECK(s[0] == -7 && s[1] == -1 && s[2] == 0 && s[3] == 3 && s[4] == 5);

    VulpVec u{};
    vulp_vec_alloc(&u, 3, 1, 1);
    u.data[0] = static_cast<char>(200);
    u.data[1] = 5;
    u.data[2] = static_cast<char>(130);
    vulp_sort(&u, VULP_SORT_U8);
    CHECK(static_cast<unsigned char>(u.data[0]) == 5 && static_cast<unsigned char>(u.data[2]) == 200);

    VulpVec strs{};
    vulp_vec_alloc(&strs, 3, sizeof(VulpVec), 1);
    auto* p = reinterpret_cast<VulpVec*>(strs.data);
    p[0] = make_str("pear");
    p[1] = make_str("apple");
    p[2] = make_str("app");
    vulp_sort(&strs, VULP_SORT_STR);
    CHECK_EQ(str(p[0]), std::string("app"));
    CHECK_EQ(str(p[1]), std::string("apple"));
    CHECK_EQ(str(p[2]), std::string("pear"));
}

// ---- 4.4 maps ----

TEST_CASE("runtime: integer maps insert defaults and iterate in key order") {
    void* m = nullptr;
    CHECK_EQ(vulp_map_len(m), 0);
    CHECK_EQ(vulp_map_has_i(m, 3), 0);
    *static_cast<int64_t*>(vulp_map_index_i(&m, VULP_KEY_SIGNED, 8, 3)) += 10;
    *static_cast<int64_t*>(vulp_map_index_i(&m, VULP_KEY_SIGNED, 8, -5)) = 1;
    *static_cast<int64_t*>(vulp_map_index_i(&m, VULP_KEY_SIGNED, 8, 3)) += 5;
    CHECK(m != nullptr);
    CHECK_EQ(vulp_map_len(m), 2);
    CHECK_EQ(*static_cast<int64_t*>(vulp_map_index_i(&m, VULP_KEY_SIGNED, 8, 3)), 15);
    CHECK_EQ(*static_cast<int64_t*>(vulp_map_index_i(&m, VULP_KEY_SIGNED, 8, 100)), 0); // default inserted
    CHECK_EQ(vulp_map_len(m), 3);
    VulpVec keys{};
    vulp_map_keys(m, &keys);
    const auto* k = reinterpret_cast<int64_t*>(keys.data);
    CHECK(keys.len == 3 && k[0] == -5 && k[1] == 3 && k[2] == 100);
    vulp_map_erase_i(m, 3);
    CHECK_EQ(vulp_map_has_i(m, 3), 0);
    CHECK_EQ(vulp_map_len(m), 2);
    vulp_map_clear(m);
    CHECK_EQ(vulp_map_len(m), 0);
}

TEST_CASE("runtime: unsigned maps order by unsigned value") {
    void* m = nullptr;
    vulp_map_index_i(&m, VULP_KEY_UNSIGNED, 8, -1); // 2^64 - 1
    vulp_map_index_i(&m, VULP_KEY_UNSIGNED, 8, 5);
    VulpVec keys{};
    vulp_map_keys(m, &keys);
    const auto* k = reinterpret_cast<uint64_t*>(keys.data);
    CHECK(k[0] == 5 && k[1] == UINT64_MAX);
}

TEST_CASE("runtime: string maps and cloning") {
    void* m = nullptr;
    const VulpVec b = make_str("banana"), a = make_str("apple"), empty{};
    *static_cast<int32_t*>(vulp_map_index_s(&m, 4, &b)) = 2;
    *static_cast<int32_t*>(vulp_map_index_s(&m, 4, &a)) = 1;
    *static_cast<int32_t*>(vulp_map_index_s(&m, 4, &empty)) = 9;
    CHECK_EQ(vulp_map_has_s(m, &a), 1);
    VulpVec keys{};
    vulp_map_keys(m, &keys);
    const auto* k = reinterpret_cast<VulpVec*>(keys.data);
    CHECK_EQ(keys.len, 3);
    CHECK_EQ(str(k[0]), std::string(""));
    CHECK_EQ(str(k[1]), std::string("apple"));
    CHECK_EQ(str(k[2]), std::string("banana"));

    void* copy = vulp_map_clone(m);
    *static_cast<int32_t*>(vulp_map_index_s(&copy, 4, &a)) = 100;
    CHECK_EQ(*static_cast<int32_t*>(vulp_map_index_s(&m, 4, &a)), 1); // original untouched
    vulp_map_erase_s(copy, &b);
    CHECK_EQ(vulp_map_len(copy), 2);
    CHECK_EQ(vulp_map_len(m), 3);
    CHECK(vulp_map_clone(nullptr) == nullptr);
}

TEST_CASE("runtime: map lookups without insertion, erase, free") {
    void* m = nullptr;
    CHECK(vulp_map_get_i(m, 1) == nullptr);
    *static_cast<int64_t*>(vulp_map_index_i(&m, VULP_KEY_SIGNED, 8, 1)) = 11;
    CHECK_EQ(*static_cast<int64_t*>(vulp_map_get_i(m, 1)), 11);
    CHECK(vulp_map_get_i(m, 2) == nullptr);
    CHECK_EQ(vulp_map_len(m), 1); // get never inserts
    const VulpVec k = make_str("k");
    void* sm = nullptr;
    CHECK(vulp_map_get_s(sm, &k) == nullptr);
    vulp_map_index_s(&sm, 8, &k);
    CHECK(vulp_map_get_s(sm, &k) != nullptr);
    vulp_map_erase_s(sm, &k);
    CHECK(vulp_map_get_s(sm, &k) == nullptr);
    vulp_map_erase_i(m, 99); // missing key: no-op
    *static_cast<int64_t*>(vulp_map_index_i(&m, VULP_KEY_SIGNED, 8, -4)) = 7;
    VulpVec vals{};
    vulp_map_values(m, &vals);
    const auto* vp = reinterpret_cast<int64_t**>(vals.data);
    CHECK(vals.len == 2 && *vp[0] == 7 && *vp[1] == 11); // key order: -4, 1
    vulp_map_free(m);
    vulp_map_free(sm);
    vulp_map_free(nullptr);
}

// ---- 4.5 memo tables ----

TEST_CASE("runtime: memo table stores, grows and clears") {
    void* memo = nullptr;
    int64_t key = 7;
    CHECK(vulp_memo_get(&memo, 8, 8, &key) == nullptr);
    for (int64_t i = 0; i < 5000; i++) {
        const int64_t v = i * i;
        vulp_memo_put(&memo, 8, 8, &i, &v);
    }
    bool all = true;
    for (int64_t i = 0; i < 5000; i++) {
        const auto* v = static_cast<int64_t*>(vulp_memo_get(&memo, 8, 8, &i));
        if (!v || *v != i * i) all = false;
    }
    CHECK(all);
    const int64_t replacement = -1;
    vulp_memo_put(&memo, 8, 8, &key, &replacement);
    CHECK_EQ(*static_cast<int64_t*>(vulp_memo_get(&memo, 8, 8, &key)), -1);
    vulp_memo_clear(memo);
    CHECK(vulp_memo_get(&memo, 8, 8, &key) == nullptr);
}

TEST_CASE("runtime: clearing a large memo table is O(1) and keeps it usable") {
    void* memo = nullptr;
    for (int64_t i = 0; i < 1000000; i++) vulp_memo_put(&memo, 8, 8, &i, &i);
    // 200000 clears of a table with ~2M slots would take minutes if clearing touched every slot
    for (int64_t round = 0; round < 200000; round++) {
        vulp_memo_clear(memo);
        const int64_t v = round * 2;
        vulp_memo_put(&memo, 8, 8, &round, &v);
        if (*static_cast<int64_t*>(vulp_memo_get(&memo, 8, 8, &round)) != v) {
            CHECK(false);
            break;
        }
    }
    const int64_t gone = 5;
    vulp_memo_clear(memo);
    CHECK(vulp_memo_get(&memo, 8, 8, &gone) == nullptr);
    for (int64_t i = 0; i < 3000; i++) vulp_memo_put(&memo, 8, 8, &i, &i); // refill after clears
    bool all = true;
    for (int64_t i = 0; i < 3000; i++) {
        const auto* v = static_cast<int64_t*>(vulp_memo_get(&memo, 8, 8, &i));
        if (!v || *v != i) all = false;
    }
    CHECK(all);
}

TEST_CASE("runtime: memo keys are compared bytewise over the whole key") {
    void* memo = nullptr;
    const char k1[3] = {1, 2, 3}, k2[3] = {1, 2, 4};
    const int32_t v1 = 10, v2 = 20;
    vulp_memo_put(&memo, 3, 4, k1, &v1);
    vulp_memo_put(&memo, 3, 4, k2, &v2);
    CHECK_EQ(*static_cast<int32_t*>(vulp_memo_get(&memo, 3, 4, k1)), 10);
    CHECK_EQ(*static_cast<int32_t*>(vulp_memo_get(&memo, 3, 4, k2)), 20);
}

// ---- 4.6 entry ----

namespace {
    int32_t deep(const int32_t n) {
        volatile char pad[512];
        pad[0] = static_cast<char>(n);
        return n == 0 ? pad[0] : deep(n - 1) + 1;
    }
    int32_t deep_entry() {
        return deep(200000) == 200000 ? 3 : 4; // ~100 MB of stack: needs the big stack
    }
} // namespace

TEST_CASE("runtime: vulp_rt_start runs on a large stack and returns the exit code") {
    CHECK_EQ(vulp_rt_start(deep_entry), 3);
}
