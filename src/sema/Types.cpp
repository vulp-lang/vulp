#include "sema/Types.hpp"

namespace Sema {
    std::string Type::str() const {
        switch (kind) {
            case TypeKind::Void: return "void";
            case TypeKind::Bool: return "bool";
            case TypeKind::Int: return std::string(is_signed ? "i" : "u") + std::to_string(bits);
            case TypeKind::Char: return "char";
            case TypeKind::Float: return "float";
            case TypeKind::String: return "string";
            case TypeKind::Vec: return "vec<" + elem->str() + ">";
            case TypeKind::Arr: return elem->str() + "[]";
            case TypeKind::Map: return "map<" + key->str() + ", " + val->str() + ">";
            case TypeKind::Struct: return st->name;
            case TypeKind::Error: return "<error>";
        }
        return "<?>";
    }

    std::string Type::mangle() const {
        switch (kind) {
            case TypeKind::Vec: return "vec." + elem->mangle();
            case TypeKind::Arr: return "arr." + elem->mangle();
            case TypeKind::Map: return "map." + key->mangle() + "." + val->mangle();
            case TypeKind::Struct: return "S." + st->name;
            default: return str();
        }
    }

    const Type* Type::element() const {
        return elem; // for strings TypeContext sets elem = char_t
    }

    bool Type::is_trivial() const {
        switch (kind) {
            case TypeKind::String:
            case TypeKind::Vec:
            case TypeKind::Arr:
            case TypeKind::Map:
                return false;
            case TypeKind::Struct:
                for (const auto& f : st->fields) {
                    if (!f.type->is_trivial()) return false;
                }
                return true;
            default:
                return true;
        }
    }

    int StructInfo::field_index(const std::string& field) const {
        for (size_t i = 0; i < fields.size(); i++) {
            if (fields[i].name == field) return static_cast<int>(i);
        }
        return -1;
    }

    TypeContext::TypeContext() {
        void_t = intern(Type{TypeKind::Void});
        Type b{TypeKind::Bool};
        b.bits = 1;
        bool_t = intern(b);
        Type c{TypeKind::Char};
        c.bits = 8;
        char_t = intern(c);
        Type f{TypeKind::Float};
        f.bits = 64;
        float_t = intern(f);
        Type s{TypeKind::String};
        s.elem = char_t;
        string_t = intern(s);
        error_t = intern(Type{TypeKind::Error});
        int_t = int_type(64, true);
    }

    const Type* TypeContext::intern(Type t) {
        storage.push_back(std::make_unique<Type>(std::move(t)));
        return storage.back().get();
    }

    const Type* TypeContext::int_type(const int bits, const bool is_signed) {
        const auto key = std::make_tuple(static_cast<int>(TypeKind::Int), bits, is_signed);
        if (const auto it = scalar_cache.find(key); it != scalar_cache.end()) return it->second;
        Type t{TypeKind::Int};
        t.bits = bits;
        t.is_signed = is_signed;
        const Type* result = intern(t);
        scalar_cache[key] = result;
        return result;
    }

    const Type* TypeContext::vec_of(const Type* elem) {
        const auto key = std::make_tuple(static_cast<int>(TypeKind::Vec), elem, nullptr);
        if (const auto it = compound_cache.find(key); it != compound_cache.end()) return it->second;
        Type t{TypeKind::Vec};
        t.elem = elem;
        const Type* result = intern(t);
        compound_cache[key] = result;
        return result;
    }

    const Type* TypeContext::arr_of(const Type* elem) {
        const auto key = std::make_tuple(static_cast<int>(TypeKind::Arr), elem, nullptr);
        if (const auto it = compound_cache.find(key); it != compound_cache.end()) return it->second;
        Type t{TypeKind::Arr};
        t.elem = elem;
        const Type* result = intern(t);
        compound_cache[key] = result;
        return result;
    }

    const Type* TypeContext::map_of(const Type* key_t, const Type* val_t) {
        const auto key = std::make_tuple(static_cast<int>(TypeKind::Map), key_t, val_t);
        if (const auto it = compound_cache.find(key); it != compound_cache.end()) return it->second;
        Type t{TypeKind::Map};
        t.key = key_t;
        t.val = val_t;
        const Type* result = intern(t);
        compound_cache[key] = result;
        return result;
    }

    const Type* TypeContext::struct_type(StructInfo* st) {
        if (const auto it = struct_cache.find(st); it != struct_cache.end()) return it->second;
        Type t{TypeKind::Struct};
        t.st = st;
        const Type* result = intern(t);
        struct_cache[st] = result;
        return result;
    }

    int TypeContext::round_bits(const int requested) {
        for (const int candidate : {8, 16, 32, 64, 128, 256}) {
            if (requested <= candidate) return candidate;
        }
        return 256;
    }

    std::optional<int> TypeContext::range_bits(const llvm::APInt& lo, const llvm::APInt& hi) {
        // getSignificantBits(): bits needed to hold the value as a signed integer.
        const unsigned needed = std::max(lo.getSignificantBits(), hi.getSignificantBits());
        if (needed > 256) return std::nullopt;
        return round_bits(static_cast<int>(needed));
    }
} // namespace Sema
