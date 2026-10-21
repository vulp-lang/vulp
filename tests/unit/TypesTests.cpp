#include "TestFramework.hpp"
#include "sema/Types.hpp"

using namespace Sema;

namespace {
    llvm::APInt big(const std::string& decimal) {
        const bool neg = decimal[0] == '-';
        const std::string digits = neg ? decimal.substr(1) : decimal;
        llvm::APInt v(512, llvm::StringRef(digits), 10);
        if (neg) v.negate();
        return v;
    }
} // namespace

TEST_CASE("types: iN widths round up to the next power of two in [8, 256]") {
    CHECK_EQ(TypeContext::round_bits(1), 8);
    CHECK_EQ(TypeContext::round_bits(7), 8);
    CHECK_EQ(TypeContext::round_bits(8), 8);
    CHECK_EQ(TypeContext::round_bits(9), 16);
    CHECK_EQ(TypeContext::round_bits(21), 32);
    CHECK_EQ(TypeContext::round_bits(33), 64);
    CHECK_EQ(TypeContext::round_bits(64), 64);
    CHECK_EQ(TypeContext::round_bits(65), 128);
    CHECK_EQ(TypeContext::round_bits(100), 128);
    CHECK_EQ(TypeContext::round_bits(129), 256);
    CHECK_EQ(TypeContext::round_bits(256), 256);
}

TEST_CASE("types: range ints pick the smallest signed power-of-two width") {
    CHECK_EQ(*TypeContext::range_bits(big("0"), big("127")), 8);
    CHECK_EQ(*TypeContext::range_bits(big("-128"), big("127")), 8);
    CHECK_EQ(*TypeContext::range_bits(big("0"), big("255")), 16); // 255 needs 9 signed bits
    CHECK_EQ(*TypeContext::range_bits(big("-129"), big("0")), 16);
    CHECK_EQ(*TypeContext::range_bits(big("0"), big("1000000007")), 32);
    CHECK_EQ(*TypeContext::range_bits(big("-2147483648"), big("2147483647")), 32);
    CHECK_EQ(*TypeContext::range_bits(big("0"), big("2147483648")), 64);
    CHECK_EQ(*TypeContext::range_bits(big("0"), big("1000000000000000000")), 64);
    CHECK_EQ(*TypeContext::range_bits(big("0"), big("9223372036854775808")), 128); // 2^63
    CHECK_EQ(*TypeContext::range_bits(big("0"), big("100000000000000000000000000000000000000")), 128); // 1e38
    CHECK_EQ(*TypeContext::range_bits(big("0"), big("1000000000000000000000000000000000000000")), 256); // 1e39
    // 2^255 does not fit in a signed 256-bit integer
    llvm::APInt two_255 = llvm::APInt::getOneBitSet(512, 255);
    CHECK(!TypeContext::range_bits(big("0"), two_255).has_value());
}

TEST_CASE("types: interning makes structurally equal types identical") {
    TypeContext t;
    CHECK(t.vec_of(t.int_t) == t.vec_of(t.int_t));
    CHECK(t.vec_of(t.int_t) != t.arr_of(t.int_t));
    CHECK(t.int_type(64, true) == t.int_t);
    CHECK(t.int_type(32, true) != t.int_type(32, false));
    CHECK(t.map_of(t.string_t, t.int_t) == t.map_of(t.string_t, t.int_t));
    CHECK(t.map_of(t.string_t, t.int_t) != t.map_of(t.int_t, t.string_t));
    CHECK(t.vec_of(t.vec_of(t.char_t)) == t.vec_of(t.vec_of(t.char_t)));
}

TEST_CASE("types: spelling and mangling") {
    TypeContext t;
    CHECK_EQ(t.int_t->str(), std::string("i64"));
    CHECK_EQ(t.int_type(8, false)->str(), std::string("u8"));
    CHECK_EQ(t.vec_of(t.int_t)->str(), std::string("vec<i64>"));
    CHECK_EQ(t.arr_of(t.arr_of(t.char_t))->str(), std::string("char[][]"));
    CHECK_EQ(t.map_of(t.string_t, t.vec_of(t.int_t))->str(), std::string("map<string, vec<i64>>"));
    CHECK_EQ(t.map_of(t.string_t, t.vec_of(t.int_t))->mangle(), std::string("map.string.vec.i64"));
    CHECK_EQ(t.arr_of(t.float_t)->mangle(), std::string("arr.float"));
}

TEST_CASE("types: trivial vs owning types") {
    TypeContext t;
    CHECK(t.int_t->is_trivial());
    CHECK(t.float_t->is_trivial());
    CHECK(!t.string_t->is_trivial());
    CHECK(!t.vec_of(t.int_t)->is_trivial());
    CHECK(!t.map_of(t.int_t, t.int_t)->is_trivial());

    StructInfo plain;
    plain.name = "P";
    plain.fields = {{"x", t.int_t, {}}, {"y", t.float_t, {}}};
    CHECK(t.struct_type(&plain)->is_trivial());

    StructInfo owning;
    owning.name = "Q";
    owning.fields = {{"x", t.int_t, {}}, {"tags", t.vec_of(t.int_t), {}}};
    CHECK(!t.struct_type(&owning)->is_trivial());
}

TEST_CASE("types: classification helpers") {
    TypeContext t;
    CHECK(t.char_t->is_integral());
    CHECK(t.char_t->is_numeric());
    CHECK(!t.bool_t->is_numeric());
    CHECK(t.bool_t->is_scalar());
    CHECK(t.string_t->is_sequence());
    CHECK(t.string_t->element() == t.char_t);
    CHECK(t.vec_of(t.int_t)->element() == t.int_t);
    CHECK(!t.map_of(t.int_t, t.int_t)->is_sequence());
}
