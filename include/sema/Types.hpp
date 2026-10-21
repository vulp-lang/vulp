#ifndef VULP_TYPES_HPP
#define VULP_TYPES_HPP

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/StringRef.h>

#include "misc/Diagnostics.hpp"

namespace AST {
    struct FnDecl;
    struct StructDecl;
}

namespace Sema {
    enum class TypeKind {
        Void,   // no value (functions without `->`)
        Bool,
        Int,    // signed or unsigned, bits in {8, 16, 32, 64, 128, 256}
        Char,   // 8-bit unsigned character
        Float,  // 64-bit IEEE double
        String, // mutable byte string, value semantics
        Vec,    // growable array, value semantics
        Arr,    // fixed-length array (length chosen at runtime), value semantics
        Map,    // ordered map, value semantics
        Struct,
        Error,  // produced after a reported error; compatible with everything to avoid cascades
    };

    struct StructInfo;

    // Types are interned by TypeContext, so two types are equal iff their pointers are equal.
    struct Type {
        TypeKind kind = TypeKind::Error;
        int bits = 0;           // Int: storage width; Char: 8; Bool: 1; Float: 64
        bool is_signed = false; // Int
        const Type* elem = nullptr; // Vec / Arr
        const Type* key = nullptr;  // Map
        const Type* val = nullptr;  // Map
        StructInfo* st = nullptr;   // Struct

        [[nodiscard]] std::string str() const;    // user-facing spelling, e.g. "vec<i64>"
        [[nodiscard]] std::string mangle() const; // identifier-safe spelling, e.g. "vec.i64"

        [[nodiscard]] bool is_int() const { return kind == TypeKind::Int; }
        [[nodiscard]] bool is_integral() const { return kind == TypeKind::Int || kind == TypeKind::Char; }
        [[nodiscard]] bool is_numeric() const { return is_integral() || kind == TypeKind::Float; }
        [[nodiscard]] bool is_scalar() const { return is_numeric() || kind == TypeKind::Bool; }
        [[nodiscard]] bool is_error() const { return kind == TypeKind::Error; }
        // String, Vec and Arr share one runtime representation: {data, len, cap}.
        [[nodiscard]] bool is_sequence() const {
            return kind == TypeKind::String || kind == TypeKind::Vec || kind == TypeKind::Arr;
        }
        // Element type of a sequence (char for strings).
        [[nodiscard]] const Type* element() const;
        // True when copying the value is a plain bit copy (no owned heap memory inside).
        [[nodiscard]] bool is_trivial() const;
    };

    struct StructField {
        std::string name;
        const Type* type = nullptr;
        SourceLoc loc;
    };

    struct StructInfo {
        std::string name;
        std::vector<StructField> fields;
        const Type* type = nullptr;
        AST::StructDecl* decl = nullptr;
        bool fields_resolved = false;

        [[nodiscard]] int field_index(const std::string& field) const;
    };

    class TypeContext {
    private:
        std::vector<std::unique_ptr<Type>> storage;
        std::map<std::tuple<int, int, bool>, const Type*> scalar_cache;
        std::map<std::tuple<int, const Type*, const Type*>, const Type*> compound_cache;
        std::map<const StructInfo*, const Type*> struct_cache;

        const Type* intern(Type t);

    public:
        TypeContext();

        const Type* void_t;
        const Type* bool_t;
        const Type* char_t;
        const Type* float_t;
        const Type* string_t;
        const Type* error_t;
        const Type* int_t; // i64

        // bits must already be a power of two in [8, 256]
        const Type* int_type(int bits, bool is_signed);
        const Type* vec_of(const Type* elem);
        const Type* arr_of(const Type* elem);
        const Type* map_of(const Type* key, const Type* val);
        const Type* struct_type(StructInfo* st);

        // Rounds a requested bit width (1..256) up to the next of {8, 16, 32, 64, 128, 256}.
        static int round_bits(int requested);
        // Smallest signed width in {8, ..., 256} that holds every value of [lo, hi].
        // Returns nullopt if the range needs more than 256 bits.
        static std::optional<int> range_bits(const llvm::APInt& lo, const llvm::APInt& hi);
    };

    // Variables (locals, globals, parameters, loop variables).
    struct FnInfo;
    struct VarInfo {
        std::string name;
        const Type* type = nullptr;
        SourceLoc loc;
        bool is_global = false;
        bool is_param = false;
        bool is_ref = false;     // `T&` parameter: storage is the caller's
        FnInfo* owner = nullptr; // enclosing function; nullptr for globals and top-level code
        int id = 0;
    };

    struct FnInfo {
        std::string name;
        AST::FnDecl* decl = nullptr;
        std::vector<VarInfo*> params;
        const Type* ret = nullptr;
        bool is_dp = false;
        SourceLoc loc;

        // Purity facts. `direct_*` are about the body itself; the others are transitive over calls.
        bool direct_reads_global = false, direct_writes_global = false, direct_io = false;
        bool reads_global = false, writes_global = false, does_io = false;
        SourceLoc first_global_write, first_io;
        std::set<FnInfo*> callees;
        // param_written[i]: the function writes through its reference parameter i
        std::vector<bool> param_written;
        // The globals the function modifies (directly, and including callees).
        std::set<VarInfo*> direct_written_globals, written_globals;
        // ref_hazards[i]: globals that reference parameter i must not alias, because the
        // function passes an element of it by reference to code that modifies that global.
        std::vector<std::set<VarInfo*>> ref_hazards;

        // dp functions that (transitively) read globals get their cache cleared before
        // every TC iteration.
        bool reset_cache_per_testcase = false;
    };

    // Everything the back end needs from semantic analysis.
    struct ProgramInfo {
        std::vector<std::unique_ptr<VarInfo>> vars;
        std::vector<std::unique_ptr<FnInfo>> fns;
        std::vector<std::unique_ptr<StructInfo>> structs;
        std::vector<VarInfo*> globals;
        bool has_tc = false;
    };
} // namespace Sema

#endif // VULP_TYPES_HPP
