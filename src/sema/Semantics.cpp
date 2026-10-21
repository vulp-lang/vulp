#include "sema/Semantics.hpp"

#include <functional>
#include <optional>

using namespace AST;
using namespace Sema;

namespace {
    bool is_builtin_name(const std::string& name) {
        return name == "min" || name == "max" || name == "abs" || name == "exit";
    }

    // Parses an integer literal / range bound ("123", "-5", "0xFF") into a 512-bit signed APInt.
    // Returns false when the literal is far too large to be a valid Vulp integer.
    bool parse_int_text(std::string text, llvm::APInt& out) {
        bool negative = false;
        if (!text.empty() && text[0] == '-') {
            negative = true;
            text = text.substr(1);
        }
        unsigned radix = 10;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            radix = 16;
            text = text.substr(2);
        }
        if (text.empty() || (radix == 10 && text.size() > 150) || (radix == 16 && text.size() > 120)) return false;
        out = llvm::APInt(512, llvm::StringRef(text), static_cast<uint8_t>(radix));
        if (negative) out.negate();
        return true;
    }

    std::string token_str(const TokenType t) {
        return std::string("'") + token_type_to_string(t) + "'";
    }
} // namespace

SemanticAnalyzer::SemanticAnalyzer(Program& p_program, Diagnostics& p_diag, TypeContext& p_types)
    : program(p_program), diag(p_diag), types(p_types) {
    push_scope(); // global scope
}

void SemanticAnalyzer::report(const SemaError& e) {
    error_count++;
    diag.error("semantic", e.loc, e.what());
}

template<class F>
void SemanticAnalyzer::guard(F&& fn) {
    try {
        fn();
    } catch (const SemaError& e) {
        report(e);
    }
}

bool SemanticAnalyzer::analyze() {
    register_structs();
    resolve_struct_fields();
    register_functions();
    for (auto& item : program.items) {
        guard([&] { check_stmt(*item); });
    }
    propagate_purity();
    return error_count == 0;
}

/*** registration ***/

void SemanticAnalyzer::register_structs() {
    for (auto& item : program.items) {
        auto* decl = std::get_if<StructDecl>(&item->node);
        if (!decl) continue;
        guard([&] {
            if (structs.contains(decl->name)) throw SemaError(item->loc, "redefinition of struct '" + decl->name + "'");
            if (is_builtin_name(decl->name)) {
                throw SemaError(item->loc, "'" + decl->name + "' is a builtin function name");
            }
            auto st = std::make_unique<StructInfo>();
            st->name = decl->name;
            st->decl = decl;
            st->type = types.struct_type(st.get());
            decl->info = st.get();
            structs[decl->name] = st.get();
            info.structs.push_back(std::move(st));
        });
    }
}

void SemanticAnalyzer::resolve_struct_fields() {
    for (const auto& st : info.structs) {
        for (auto& field : st->decl->fields) {
            const Type* t = types.error_t;
            guard([&] {
                if (st->field_index(field.name) >= 0) {
                    throw SemaError(field.loc, "duplicate field '" + field.name + "' in struct '" + st->name + "'");
                }
                t = resolve_type(field.type, false);
            });
            st->fields.push_back({field.name, t, field.loc});
        }
        st->fields_resolved = true;
    }

    // A struct may not contain itself by value (directly or through other structs).
    std::function<bool(const Type*, const StructInfo*, std::set<const StructInfo*>&)> contains =
        [&](const Type* t, const StructInfo* target, std::set<const StructInfo*>& seen) -> bool {
        if (t->kind != TypeKind::Struct) return false; // containers live on the heap
        if (t->st == target) return true;
        if (!seen.insert(t->st).second) return false;
        for (const auto& f : t->st->fields) {
            if (contains(f.type, target, seen)) return true;
        }
        return false;
    };
    for (const auto& st : info.structs) {
        for (auto& f : st->fields) {
            std::set<const StructInfo*> seen;
            if (contains(f.type, st.get(), seen)) {
                report(SemaError(f.loc, "struct '" + st->name + "' contains itself by value through field '"
                                            + f.name + "'; use vec<" + st->name + "> instead"));
                f.type = types.error_t;
            }
        }
    }
}

void SemanticAnalyzer::register_functions() {
    for (auto& item : program.items) {
        auto* decl = std::get_if<FnDecl>(&item->node);
        if (!decl) continue;
        guard([&] {
            if (functions.contains(decl->name)) {
                throw SemaError(item->loc, "redefinition of function '" + decl->name + "'");
            }
            if (is_builtin_name(decl->name)) {
                throw SemaError(item->loc, "'" + decl->name + "' is a builtin function and cannot be redefined");
            }
            if (structs.contains(decl->name)) {
                throw SemaError(item->loc, "function '" + decl->name + "' has the same name as a struct");
            }
            auto fn = std::make_unique<FnInfo>();
            fn->name = decl->name;
            fn->decl = decl;
            fn->is_dp = decl->is_dp;
            fn->loc = item->loc;
            for (auto& p : decl->params) {
                const Type* t = types.error_t;
                guard([&] { t = resolve_type(p.type, false); });
                auto var = std::make_unique<VarInfo>();
                var->name = p.name;
                var->type = t;
                var->loc = p.loc;
                var->is_param = true;
                var->is_ref = p.type.is_ref;
                var->owner = fn.get();
                var->id = static_cast<int>(info.vars.size());
                p.var = var.get();
                fn->params.push_back(var.get());
                info.vars.push_back(std::move(var));
            }
            fn->param_written.assign(fn->params.size(), false);
            fn->ret = types.void_t;
            if (decl->ret) guard([&] { fn->ret = resolve_type(*decl->ret, false); });

            if (fn->is_dp) {
                for (const auto* p : fn->params) {
                    if (p->is_ref) {
                        report(SemaError(p->loc, "parameters of dp function '" + fn->name
                                                     + "' cannot be references"));
                    } else if (!p->type->is_error() && !p->type->is_integral() && p->type->kind != TypeKind::Bool) {
                        report(SemaError(p->loc, "parameters of dp function '" + fn->name
                                                     + "' must be integers, chars or bools (they form the cache "
                                                       "key), found " + p->type->str()));
                    }
                }
                if (!fn->ret->is_error() && !fn->ret->is_scalar()) {
                    report(SemaError(item->loc, "dp function '" + fn->name
                                                    + "' must return a scalar value (integer, char, bool or float)"));
                }
            }

            decl->info = fn.get();
            functions[decl->name] = fn.get();
            info.fns.push_back(std::move(fn));
        });
    }
}

void SemanticAnalyzer::propagate_purity() {
    for (const auto& fn : info.fns) {
        fn->reads_global = fn->direct_reads_global;
        fn->does_io = fn->direct_io;
        fn->written_globals = fn->direct_written_globals;
        fn->ref_hazards.assign(fn->params.size(), {});
    }
    const auto merge = [](std::set<VarInfo*>& into, const std::set<VarInfo*>& from) {
        const size_t before = into.size();
        into.insert(from.begin(), from.end());
        return into.size() != before;
    };
    const auto param_index = [](const FnInfo* fn, const VarInfo* var) -> std::optional<size_t> {
        for (size_t i = 0; i < fn->params.size(); i++) {
            if (fn->params[i] == var) return i;
        }
        return std::nullopt;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& fn : info.fns) {
            for (const FnInfo* callee : fn->callees) {
                if (callee->reads_global && !fn->reads_global) fn->reads_global = changed = true;
                if (callee->does_io && !fn->does_io) fn->does_io = changed = true;
                if (merge(fn->written_globals, callee->written_globals)) changed = true;
            }
        }
        for (const auto& ra : ref_args) {
            if (!ra.caller) continue;
            const auto caller_param = ra.root->is_ref && ra.root->owner == ra.caller ? param_index(ra.caller, ra.root)
                                                                                      : std::nullopt;
            // Passing a variable to a reference parameter the callee writes through is a write.
            if (ra.callee->param_written[ra.param_index]) {
                if (ra.root->is_global && ra.caller->written_globals.insert(ra.root).second) changed = true;
                if (caller_param && !ra.caller->param_written[*caller_param]) {
                    ra.caller->param_written[*caller_param] = true;
                    changed = true;
                }
            }
            // An element of a reference parameter handed to code that modifies global G means
            // the parameter must never alias G; forwarding a parameter inherits the callee's rule.
            if (caller_param) {
                auto& hazards = ra.caller->ref_hazards[*caller_param];
                if (ra.through_element && merge(hazards, ra.callee->written_globals)) changed = true;
                if (merge(hazards, ra.callee->ref_hazards[ra.param_index])) changed = true;
            }
        }
    }
    for (const auto& fn : info.fns) fn->writes_global = !fn->written_globals.empty();

    // A reference into a global container must not outlive a modification of that global.
    for (const auto& ra : ref_args) {
        if (!ra.root->is_global) continue;
        if (ra.through_element && ra.callee->written_globals.contains(ra.root)) {
            report(SemaError(ra.loc, "cannot pass an element of global '" + ra.root->name + "' by reference to '"
                                         + ra.callee->name + "', which modifies '" + ra.root->name
                                         + "' (the element could be freed while it is referenced)"));
        } else if (ra.callee->ref_hazards[ra.param_index].contains(ra.root)) {
            report(SemaError(ra.loc, "cannot pass global '" + ra.root->name + "' by reference to '"
                                         + ra.callee->name + "': it passes part of it by reference to code "
                                         + "that modifies '" + ra.root->name + "'"));
        }
    }

    for (const auto& fn : info.fns) {
        if (!fn->is_dp) continue;
        if (fn->writes_global) {
            const SourceLoc where = fn->direct_writes_global ? fn->first_global_write : fn->loc;
            report(SemaError(where, "dp function '" + fn->name
                                        + "' must not modify global variables (its results are cached)"));
        }
        if (fn->does_io) {
            const SourceLoc where = fn->direct_io ? fn->first_io : fn->loc;
            report(SemaError(where, "dp function '" + fn->name + "' must not perform input/output"));
        }
        fn->reset_cache_per_testcase = fn->reads_global;
    }
}

/*** scopes & symbols ***/

VarInfo* SemanticAnalyzer::declare_var(const std::string& name, const Type* type, const SourceLoc loc) {
    auto& scope = scopes.back();
    if (scope.contains(name)) throw SemaError(loc, "redeclaration of '" + name + "' in the same scope");
    if (structs.contains(name)) throw SemaError(loc, "variable '" + name + "' has the same name as a struct");
    auto var = std::make_unique<VarInfo>();
    var->name = name;
    var->type = type;
    var->loc = loc;
    var->is_global = at_global_scope();
    var->owner = current_fn;
    var->id = static_cast<int>(info.vars.size());
    VarInfo* result = var.get();
    scope[name] = result;
    if (result->is_global) info.globals.push_back(result);
    info.vars.push_back(std::move(var));
    return result;
}

VarInfo* SemanticAnalyzer::lookup_var(const std::string& name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        if (const auto found = it->find(name); found != it->end()) return found->second;
    }
    return nullptr;
}

/*** types ***/

const Type* SemanticAnalyzer::resolve_base_type(const TypeSpec& spec) {
    switch (spec.kind) {
        case TypeSpec::Kind::Int: return types.int_t;
        case TypeSpec::Kind::IntN: return types.int_type(TypeContext::round_bits(spec.bits), spec.is_signed);
        case TypeSpec::Kind::RangeInt: {
            llvm::APInt lo, hi;
            if (!parse_int_text(spec.lo, lo) || !parse_int_text(spec.hi, hi)) {
                throw SemaError(spec.loc, "range bound is too large");
            }
            if (lo.sgt(hi)) throw SemaError(spec.loc, "empty range: int<" + spec.lo + ".." + spec.hi + ">");
            const auto bits = TypeContext::range_bits(lo, hi);
            if (!bits) throw SemaError(spec.loc, "range int<" + spec.lo + ".." + spec.hi + "> needs more than 256 bits");
            return types.int_type(*bits, true);
        }
        case TypeSpec::Kind::Char: return types.char_t;
        case TypeSpec::Kind::Bool: return types.bool_t;
        case TypeSpec::Kind::Float: return types.float_t;
        case TypeSpec::Kind::String: return types.string_t;
        case TypeSpec::Kind::Vec: {
            auto& arg = const_cast<TypeSpec&>(spec.args[0]);
            return types.vec_of(resolve_type(arg, false));
        }
        case TypeSpec::Kind::Map: {
            auto& key_spec = const_cast<TypeSpec&>(spec.args[0]);
            auto& val_spec = const_cast<TypeSpec&>(spec.args[1]);
            const Type* key = resolve_type(key_spec, false);
            const Type* val = resolve_type(val_spec, false);
            const bool ok_key = (key->is_integral() && key->bits <= 64) || key->kind == TypeKind::Bool
                                || key->kind == TypeKind::String || key->is_error();
            if (!ok_key) {
                throw SemaError(key_spec.loc, "map keys must be integers (up to 64 bits), chars, bools or strings, found "
                                              + key->str());
            }
            return types.map_of(key, val);
        }
        case TypeSpec::Kind::Named: {
            const auto it = structs.find(spec.name);
            if (it == structs.end()) throw SemaError(spec.loc, "unknown type '" + spec.name + "'");
            return it->second->type;
        }
    }
    throw SemaError(spec.loc, "unsupported type");
}

const Type* SemanticAnalyzer::resolve_type(TypeSpec& spec, const bool allow_sizes) {
    const Type* t = resolve_base_type(spec);
    bool seen_unsized = false;
    for (const auto& dim : spec.dims) {
        if (dim) {
            if (!allow_sizes) throw SemaError(dim->loc, "array sizes are not allowed here; use '[]'");
            if (seen_unsized) throw SemaError(dim->loc, "a sized array dimension cannot follow an unsized one");
        } else {
            seen_unsized = true;
        }
    }
    for (size_t i = 0; i < spec.dims.size(); i++) t = types.arr_of(t);
    return t;
}

bool SemanticAnalyzer::assignable(const Type* to, const Type* from) const {
    if (to == from) return true;
    if (to->is_error() || from->is_error()) return true;
    if (to->is_integral() && from->is_integral()) return true;
    if (to->kind == TypeKind::Float && from->is_integral()) return true;
    const bool to_seq = to->kind == TypeKind::Vec || to->kind == TypeKind::Arr;
    const bool from_seq = from->kind == TypeKind::Vec || from->kind == TypeKind::Arr;
    if (to_seq && from_seq && to->elem == from->elem) return true;
    return false;
}

bool SemanticAnalyzer::is_int_literal(const Expr& e) {
    if (std::holds_alternative<IntLit>(e.node)) return true;
    if (const auto* u = std::get_if<UnaryExpr>(&e.node)) {
        return u->op == TokenType::MINUS && is_int_literal(*u->operand);
    }
    return false;
}

namespace {
    // Value of an integer literal expression (IntLit or -IntLit) as a 512-bit signed integer.
    llvm::APInt literal_value(const Expr& e) {
        if (const auto* lit = std::get_if<IntLit>(&e.node)) return lit->value;
        const auto& u = std::get<UnaryExpr>(e.node);
        llvm::APInt v = literal_value(*u.operand);
        v.negate();
        return v;
    }
} // namespace

bool SemanticAnalyzer::literal_fits(const Expr& e, const Type* to) {
    if (!to->is_integral()) return false;
    const llvm::APInt v = literal_value(e);
    const unsigned bits = static_cast<unsigned>(to->bits);
    if (to->kind == TypeKind::Int && to->is_signed) {
        const llvm::APInt min = llvm::APInt::getSignedMinValue(bits).sext(512);
        const llvm::APInt max = llvm::APInt::getSignedMaxValue(bits).sext(512);
        return v.sge(min) && v.sle(max);
    }
    return v.isNonNegative() && v.getActiveBits() <= bits;
}

void SemanticAnalyzer::retype_literal(Expr& e, const Type* to) {
    e.type = to;
    if (auto* u = std::get_if<UnaryExpr>(&e.node)) retype_literal(*u->operand, to);
}

void SemanticAnalyzer::coerce(Expr& e, const Type* to, const std::string& context) {
    const Type* from = e.type;
    if (from->kind == TypeKind::Void) {
        throw SemaError(e.loc, "cannot use the result of a function that returns nothing " + context);
    }
    if (!assignable(to, from)) {
        throw SemaError(e.loc, "type mismatch " + context + ": expected " + to->str() + ", found " + from->str());
    }
    if (is_int_literal(e) && to->is_integral()) {
        if (!literal_fits(e, to)) throw SemaError(e.loc, "integer constant does not fit in " + to->str());
        retype_literal(e, to);
    }
}

// Integer promotion: chars and integers narrower than 64 bits compute as `int` (i64). Narrow
// widths are storage widths; a result wraps only when it is stored back into a narrow type.
const Type* SemanticAnalyzer::arith_type(const Type* t) const {
    if (t->kind == TypeKind::Char) return types.int_t;
    if (t->kind == TypeKind::Int && t->bits < 64) return types.int_t;
    return t;
}

// Type both operands of a binary operator are converted to. Integer literals adapt to the
// other operand's type when they fit; otherwise the wider type wins (unsigned on ties).
const Type* SemanticAnalyzer::common_type(Expr& lhs, Expr& rhs, const bool promote_char) {
    const Type* lt = promote_char ? arith_type(lhs.type) : lhs.type;
    const Type* rt = promote_char ? arith_type(rhs.type) : rhs.type;
    if (lt->kind == TypeKind::Float || rt->kind == TypeKind::Float) return types.float_t;

    const bool l_lit = is_int_literal(lhs), r_lit = is_int_literal(rhs);
    if (l_lit && !r_lit && literal_fits(lhs, rt)) {
        retype_literal(lhs, rt);
        return rt;
    }
    if (r_lit && !l_lit && literal_fits(rhs, lt)) {
        retype_literal(rhs, lt);
        return lt;
    }
    if (lt == rt) return lt;
    lt = arith_type(lt);
    rt = arith_type(rt);
    if (lt == rt) return lt;
    if (lt->bits != rt->bits) return lt->bits > rt->bits ? lt : rt;
    return lt->is_signed ? rt : lt;
}

bool SemanticAnalyzer::printable(const Type* t) const {
    switch (t->kind) {
        case TypeKind::Bool:
        case TypeKind::Int:
        case TypeKind::Char:
        case TypeKind::Float:
        case TypeKind::String:
        case TypeKind::Error:
            return true;
        case TypeKind::Vec:
        case TypeKind::Arr:
            return printable(t->elem);
        case TypeKind::Struct:
            for (const auto& f : t->st->fields) {
                if (!printable(f.type)) return false;
            }
            return true;
        default:
            return false;
    }
}

bool SemanticAnalyzer::readable(const Type* t) const {
    switch (t->kind) {
        case TypeKind::Int:
        case TypeKind::Char:
        case TypeKind::Float:
        case TypeKind::String:
        case TypeKind::Error:
            return true;
        case TypeKind::Vec:
        case TypeKind::Arr:
            return readable(t->elem);
        case TypeKind::Struct:
            for (const auto& f : t->st->fields) {
                if (!readable(f.type)) return false;
            }
            return true;
        default:
            return false;
    }
}

/*** places & purity ***/

bool SemanticAnalyzer::is_place(const Expr& e) {
    if (std::holds_alternative<NameExpr>(e.node)) return true;
    if (const auto* ix = std::get_if<IndexExpr>(&e.node)) return is_place(*ix->base);
    if (const auto* f = std::get_if<FieldExpr>(&e.node)) return is_place(*f->base);
    return false;
}

Expr* SemanticAnalyzer::place_root(Expr& e) {
    if (std::holds_alternative<NameExpr>(e.node)) return &e;
    if (auto* ix = std::get_if<IndexExpr>(&e.node)) return place_root(*ix->base);
    if (auto* f = std::get_if<FieldExpr>(&e.node)) return place_root(*f->base);
    return nullptr;
}

void SemanticAnalyzer::require_place(Expr& e, const std::string& what) {
    if (!is_place(e)) {
        throw SemaError(e.loc, "cannot " + what + " a temporary value; expected a variable, element or field");
    }
}

SemanticAnalyzer::PlacePath SemanticAnalyzer::place_path(const Expr& e) {
    if (const auto* n = std::get_if<NameExpr>(&e.node)) {
        PlacePath path;
        path.root = n->var;
        path.type = e.type;
        return path;
    }
    if (const auto* ix = std::get_if<IndexExpr>(&e.node)) {
        PlacePath path = place_path(*ix->base);
        path.steps.push_back(-1);
        path.containers.push_back(ix->base->type);
        path.through_element = true;
        if (ix->base->type && ix->base->type->kind == TypeKind::Map) path.through_map = true;
        path.type = e.type;
        return path;
    }
    const auto& f = std::get<FieldExpr>(e.node);
    PlacePath path = place_path(*f.base);
    path.steps.push_back(f.field_index);
    path.type = e.type;
    return path;
}

namespace {
    // Can a value of type `outer` contain (or be) a value of type `inner`? Structs may contain
    // themselves through containers (tree nodes), so visited structs are not explored again.
    bool type_reaches(const Type* outer, const Type* inner, std::set<const StructInfo*>& seen) {
        if (outer == inner) return true;
        switch (outer->kind) {
            case TypeKind::Vec:
            case TypeKind::Arr:
                return type_reaches(outer->elem, inner, seen);
            case TypeKind::Map:
                return type_reaches(outer->val, inner, seen);
            case TypeKind::Struct:
                if (!seen.insert(outer->st).second) return false;
                for (const auto& f : outer->st->fields) {
                    if (type_reaches(f.type, inner, seen)) return true;
                }
                return false;
            default:
                return false;
        }
    }

    bool type_reaches(const Type* outer, const Type* inner) {
        std::set<const StructInfo*> seen;
        return type_reaches(outer, inner, seen);
    }

    // Can a value of type `outer` be, or contain through struct fields only, a value of type `inner`?
    bool field_reaches(const Type* outer, const Type* inner) {
        if (outer == inner) return true;
        if (outer->kind != TypeKind::Struct) return false;
        for (const auto& f : outer->st->fields) {
            if (field_reaches(f.type, inner)) return true;
        }
        return false;
    }
} // namespace

// Two distinct variables can only be the same object (or one a field of the other) at runtime
// when both are `&` parameters of the current function or one is such a parameter and the
// other a global; locals have value semantics and never alias anything.
bool SemanticAnalyzer::may_alias(const VarInfo* a, const VarInfo* b) const {
    const auto aliasable = [&](const VarInfo* v) {
        return v->is_global || (v->is_ref && current_fn && v->owner == current_fn);
    };
    if (a == b || !aliasable(a) || !aliasable(b) || (a->is_global && b->is_global)) return false;
    return field_reaches(a->type, b->type) || field_reaches(b->type, a->type);
}

// Two reference arguments of one call may not be a container and (possibly) one of its
// elements: through the container reference the callee could reallocate, replace or erase
// the storage the element reference points into. `swap(a[i], a[j])` is fine.
void SemanticAnalyzer::check_ref_aliasing(const std::vector<std::pair<PlacePath, SourceLoc>>& refs,
                                          const std::string& callee) {
    for (const auto& [outer, outer_loc] : refs) {
        for (const auto& [inner, inner_loc] : refs) {
            if (outer.root != inner.root || outer.steps.size() >= inner.steps.size()) continue;
            bool prefix = true; // element steps may refer to the same element at runtime
            for (size_t k = 0; k < outer.steps.size() && prefix; k++) prefix = outer.steps[k] == inner.steps[k];
            bool element_below = false;
            for (size_t k = outer.steps.size(); k < inner.steps.size(); k++) {
                if (inner.steps[k] == -1) element_below = true;
            }
            if (prefix && element_below) {
                throw SemaError(inner_loc, "cannot pass an element of '" + outer.root->name
                                               + "' by reference together with "
                                               + (outer.steps.empty() ? "'" + outer.root->name + "' itself"
                                                                      : "the part of '" + outer.root->name
                                                                            + "' that contains it")
                                               + ": '" + callee + "' could resize or replace the container "
                                               + "and leave the element reference dangling");
            }
        }
    }
    // Different variables that may be the same object at runtime (e.g. after `f(v, v)` or
    // `f(G)`): reject an element of one passed together with something of the other that
    // can contain that element's container.
    for (const auto& [outer, outer_loc] : refs) {
        for (const auto& [inner, inner_loc] : refs) {
            if (outer.root == inner.root || !inner.through_element || !outer.type || outer.type->is_trivial()) continue;
            if (!may_alias(outer.root, inner.root)) continue;
            for (const Type* container : inner.containers) {
                if (!type_reaches(outer.type, container)) continue;
                throw SemaError(inner_loc, "cannot pass an element of '" + inner.root->name
                                               + "' by reference together with '" + outer.root->name + "': '"
                                               + outer.root->name + "' and '" + inner.root->name
                                               + "' may be the same object (e.g. when the caller passes the same "
                                               + "variable for both), and '" + callee
                                               + "' could then resize or replace the container and leave the "
                                               + "element reference dangling");
            }
        }
    }
}

void SemanticAnalyzer::note_write(Expr& target) {
    if (!current_fn) return;
    const Expr* root = place_root(target);
    if (!root) return;
    const VarInfo* var = std::get<NameExpr>(root->node).var;
    if (!var) return;
    if (var->is_global) {
        if (!current_fn->direct_writes_global) current_fn->first_global_write = target.loc;
        current_fn->direct_writes_global = true;
        current_fn->direct_written_globals.insert(const_cast<VarInfo*>(var));
    } else if (var->is_ref && var->owner == current_fn) {
        for (size_t i = 0; i < current_fn->params.size(); i++) {
            if (current_fn->params[i] == var) current_fn->param_written[i] = true;
        }
    }
}

void SemanticAnalyzer::note_io(const SourceLoc loc) {
    if (!current_fn) return;
    if (!current_fn->direct_io) current_fn->first_io = loc;
    current_fn->direct_io = true;
}

/*** expressions ***/

const Type* SemanticAnalyzer::check_expr(Expr& e, const Type* expected) {
    const Type* result = std::visit(overloaded{
        [&](IntLit& lit) -> const Type* { return check_int_lit(e, lit); },
        [&](FloatLit&) -> const Type* { return types.float_t; },
        [&](CharLit&) -> const Type* { return types.char_t; },
        [&](StringLit&) -> const Type* { return types.string_t; },
        [&](BoolLit&) -> const Type* { return types.bool_t; },
        [&](ArrayLit& lit) -> const Type* { return check_array_lit(e, lit, expected); },
        [&](RangeLit& lit) -> const Type* { return check_range_lit(e, lit, expected); },
        [&](NameExpr& n) -> const Type* {
            VarInfo* var = lookup_var(n.name);
            if (!var) {
                if (functions.contains(n.name) || is_builtin_name(n.name)) {
                    throw SemaError(e.loc, "'" + n.name + "' is a function; call it with " + n.name + "(...)");
                }
                throw SemaError(e.loc, "unknown variable '" + n.name + "'");
            }
            n.var = var;
            if (var->is_global && current_fn) current_fn->direct_reads_global = true;
            return var->type;
        },
        [&](UnaryExpr& u) -> const Type* { return check_unary(e, u); },
        [&](BinaryExpr& b) -> const Type* { return check_binary(e, b); },
        [&](AssignExpr& a) -> const Type* { return check_assign(e, a); },
        [&](IncDecExpr& x) -> const Type* {
            const Type* t = check_expr(*x.target);
            require_place(*x.target, std::string("apply ") + token_type_to_string(x.op) + " to");
            if (!t->is_integral() && !t->is_error()) {
                throw SemaError(e.loc, std::string(token_type_to_string(x.op)) + " needs an integer, found " + t->str());
            }
            note_write(*x.target);
            return t;
        },
        [&](CallExpr& c) -> const Type* { return check_call(e, c); },
        [&](CastExpr& c) -> const Type* { return check_cast(e, c); },
        [&](IndexExpr& ix) -> const Type* { return check_index(e, ix); },
        [&](FieldExpr& f) -> const Type* {
            const Type* bt = check_expr(*f.base);
            if (bt->is_error()) return types.error_t;
            if (bt->kind != TypeKind::Struct) {
                if (f.field == "len") throw SemaError(e.loc, "use .len() to get the length");
                throw SemaError(e.loc, "a value of type " + bt->str() + " has no fields");
            }
            f.field_index = bt->st->field_index(f.field);
            if (f.field_index < 0) {
                throw SemaError(e.loc, "struct '" + bt->st->name + "' has no field '" + f.field + "'");
            }
            return bt->st->fields[f.field_index].type;
        },
        [&](MethodCallExpr& m) -> const Type* { return check_method(e, m); },
    }, e.node);
    e.type = result;
    return result;
}

const Type* SemanticAnalyzer::check_int_lit(Expr& e, IntLit& lit) {
    llvm::APInt v;
    if (!parse_int_text(lit.text, v) || v.getActiveBits() > 256) {
        throw SemaError(e.loc, "integer literal is too large (more than 256 bits)");
    }
    lit.value = v;
    const unsigned bits = v.getActiveBits();
    if (bits <= 63) return types.int_t;
    if (bits <= 127) return types.int_type(128, true);
    if (bits <= 255) return types.int_type(256, true);
    return types.int_type(256, false);
}

namespace {
    // Unifies two element types of an array literal without promoting chars.
    const Type* unify(TypeContext& types, const Type* a, const Type* b) {
        if (a == b) return a;
        if (a->is_error()) return b;
        if (b->is_error()) return a;
        if (a->is_numeric() && b->is_numeric()) {
            if (a->kind == TypeKind::Float || b->kind == TypeKind::Float) return types.float_t;
            const Type* x = a->kind == TypeKind::Char ? types.int_t : a;
            const Type* y = b->kind == TypeKind::Char ? types.int_t : b;
            if (x == y) return x;
            if (x->bits != y->bits) return x->bits > y->bits ? x : y;
            return x->is_signed ? y : x;
        }
        return nullptr;
    }
} // namespace

const Type* SemanticAnalyzer::check_array_lit(Expr& e, ArrayLit& lit, const Type* expected) {
    const bool has_hint = expected && (expected->kind == TypeKind::Vec || expected->kind == TypeKind::Arr);
    if (has_hint) {
        for (auto& el : lit.elems) {
            check_expr(*el, expected->elem);
            coerce(*el, expected->elem, "in array literal");
        }
        return expected;
    }
    if (lit.elems.empty()) {
        throw SemaError(e.loc, "cannot infer the element type of an empty array literal");
    }
    const Type* elem = nullptr;
    for (auto& el : lit.elems) {
        const Type* t = check_expr(*el);
        if (t->kind == TypeKind::Void) throw SemaError(el->loc, "array elements must have a value");
        if (!elem) {
            elem = t;
            continue;
        }
        const Type* u = unify(types, elem, t);
        if (!u) {
            throw SemaError(el->loc, "array literal elements have incompatible types " + elem->str() + " and "
                                     + t->str());
        }
        elem = u;
    }
    for (auto& el : lit.elems) coerce(*el, elem, "in array literal");
    return types.vec_of(elem);
}

const Type* SemanticAnalyzer::check_range_lit(Expr& e, RangeLit& lit, const Type* expected) {
    const Type* lt = check_expr(*lit.lo);
    const Type* ht = check_expr(*lit.hi);
    if ((!lt->is_integral() && !lt->is_error()) || (!ht->is_integral() && !ht->is_error())) {
        throw SemaError(e.loc, "range bounds must be integers or chars, found " + lt->str() + " and " + ht->str());
    }
    if (expected && (expected->kind == TypeKind::Vec || expected->kind == TypeKind::Arr)
        && (expected->elem->is_integral() || expected->elem->kind == TypeKind::Float)) {
        coerce(*lit.lo, expected->elem, "in range literal");
        coerce(*lit.hi, expected->elem, "in range literal");
        if (expected->elem->kind == TypeKind::Float) {
            throw SemaError(e.loc, "range literals produce integers, not floats");
        }
        return expected;
    }
    const Type* t = (lt->is_error() || ht->is_error()) ? types.error_t : common_type(*lit.lo, *lit.hi, false);
    return t->is_error() ? types.error_t : types.vec_of(t);
}

const Type* SemanticAnalyzer::check_unary(Expr& e, UnaryExpr& u) {
    const Type* t = check_expr(*u.operand);
    if (t->is_error()) return t;
    switch (u.op) {
        case TokenType::MINUS:
            if (!t->is_numeric()) throw SemaError(e.loc, "unary '-' needs a number, found " + t->str());
            return arith_type(t);
        case TokenType::NOT:
            if (t->kind != TypeKind::Bool) throw SemaError(e.loc, "'!' needs a bool, found " + t->str());
            return t;
        case TokenType::TILDE:
            if (!t->is_integral()) throw SemaError(e.loc, "'~' needs an integer, found " + t->str());
            return arith_type(t);
        default:
            throw SemaError(e.loc, "unknown unary operator");
    }
}

const Type* SemanticAnalyzer::check_binary(Expr& e, BinaryExpr& b) {
    check_expr(*b.lhs);
    check_expr(*b.rhs);
    return binary_result(e, b.op, *b.lhs, *b.rhs, b.operand_type);
}

const Type* SemanticAnalyzer::binary_result(Expr& e, const TokenType op, Expr& lhs, Expr& rhs,
                                            const Type*& operand_type) {
    const Type* lt = lhs.type;
    const Type* rt = rhs.type;
    if (lt->is_error() || rt->is_error()) {
        operand_type = types.error_t;
        return types.error_t;
    }
    if (lt->kind == TypeKind::Void || rt->kind == TypeKind::Void) {
        throw SemaError(e.loc, "cannot use the result of a function that returns nothing in an expression");
    }
    const auto mismatch = [&](const std::string& what) -> SemaError {
        return SemaError(e.loc, "operator " + token_str(op) + " " + what + ", found " + lt->str() + " and " + rt->str());
    };

    switch (op) {
        case TokenType::AND_AND:
        case TokenType::OR_OR:
            if (lt->kind != TypeKind::Bool || rt->kind != TypeKind::Bool) throw mismatch("needs bool operands");
            operand_type = types.bool_t;
            return types.bool_t;

        case TokenType::EQUAL_EQUAL:
        case TokenType::NOT_EQUAL:
            if (lt->is_numeric() && rt->is_numeric()) {
                operand_type = common_type(lhs, rhs, false);
            } else if (lt->kind == TypeKind::Bool && rt->kind == TypeKind::Bool) {
                operand_type = types.bool_t;
            } else if (lt->kind == TypeKind::String && rt->kind == TypeKind::String) {
                operand_type = types.string_t;
            } else {
                throw mismatch("cannot compare these types");
            }
            return types.bool_t;

        case TokenType::LESS:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER:
        case TokenType::GREATER_EQUAL:
            if (lt->is_numeric() && rt->is_numeric()) {
                operand_type = common_type(lhs, rhs, false);
            } else if (lt->kind == TypeKind::String && rt->kind == TypeKind::String) {
                operand_type = types.string_t;
            } else {
                throw mismatch("cannot order these types");
            }
            return types.bool_t;

        case TokenType::PLUS:
            if (lt->kind == TypeKind::String && (rt->kind == TypeKind::String || rt->kind == TypeKind::Char)) {
                operand_type = types.string_t;
                return types.string_t;
            }
            [[fallthrough]];
        case TokenType::MINUS:
        case TokenType::STAR:
        case TokenType::SLASH:
            if (!lt->is_numeric() || !rt->is_numeric()) throw mismatch("needs numbers");
            operand_type = common_type(lhs, rhs, true);
            return operand_type;

        case TokenType::PERCENT:
            if (!lt->is_integral() || !rt->is_integral()) throw mismatch("needs integers");
            operand_type = common_type(lhs, rhs, true);
            return operand_type;

        case TokenType::AMP:
        case TokenType::PIPE:
        case TokenType::CARET:
            if (lt->kind == TypeKind::Bool && rt->kind == TypeKind::Bool) {
                operand_type = types.bool_t;
                return types.bool_t;
            }
            if (!lt->is_integral() || !rt->is_integral()) throw mismatch("needs integers");
            operand_type = common_type(lhs, rhs, true);
            return operand_type;

        case TokenType::SHL:
        case TokenType::SHR:
            if (!lt->is_integral() || !rt->is_integral()) throw mismatch("needs integers");
            operand_type = arith_type(lt);
            if (is_int_literal(rhs) && literal_fits(rhs, operand_type)) retype_literal(rhs, operand_type);
            return operand_type;

        default:
            throw SemaError(e.loc, "unknown binary operator " + token_str(op));
    }
}

namespace {
    TokenType compound_to_binary(const TokenType op) {
        switch (op) {
            case TokenType::PLUS_EQUAL: return TokenType::PLUS;
            case TokenType::MINUS_EQUAL: return TokenType::MINUS;
            case TokenType::STAR_EQUAL: return TokenType::STAR;
            case TokenType::SLASH_EQUAL: return TokenType::SLASH;
            case TokenType::PERCENT_EQUAL: return TokenType::PERCENT;
            case TokenType::AMP_EQUAL: return TokenType::AMP;
            case TokenType::PIPE_EQUAL: return TokenType::PIPE;
            case TokenType::CARET_EQUAL: return TokenType::CARET;
            case TokenType::SHL_EQUAL: return TokenType::SHL;
            case TokenType::SHR_EQUAL: return TokenType::SHR;
            default: return op;
        }
    }
} // namespace

const Type* SemanticAnalyzer::check_assign(Expr& e, AssignExpr& a) {
    const Type* tt = check_expr(*a.target);
    require_place(*a.target, "assign to");
    if (a.op == TokenType::EQUAL) {
        check_expr(*a.value, tt);
        coerce(*a.value, tt, "in assignment");
        note_write(*a.target);
        return tt;
    }
    check_expr(*a.value);
    const Type* result = binary_result(e, compound_to_binary(a.op), *a.target, *a.value, a.operand_type);
    if (!assignable(tt, result)) {
        throw SemaError(e.loc, std::string("cannot apply '") + token_type_to_string(a.op) + "' to " + tt->str()
                               + " and " + a.value->type->str());
    }
    note_write(*a.target);
    return tt;
}

const Type* SemanticAnalyzer::check_call(Expr& e, CallExpr& c) {
    const std::string& name = c.callee;

    if (is_builtin_name(name)) {
        c.kind = CallKind::Builtin;
        for (auto& arg : c.args) check_expr(*arg);
        if (name == "exit") {
            c.builtin = Builtin::Exit;
            if (c.args.size() != 1) throw SemaError(e.loc, "exit() takes exactly one argument (the exit code)");
            coerce(*c.args[0], types.int_type(32, true), "for exit code");
            note_io(e.loc);
            return types.void_t;
        }
        if (name == "abs") {
            c.builtin = Builtin::Abs;
            if (c.args.size() != 1) throw SemaError(e.loc, "abs() takes exactly one argument");
            const Type* t = c.args[0]->type;
            if (t->is_error()) return t;
            if (!t->is_numeric()) throw SemaError(e.loc, "abs() needs a number, found " + t->str());
            return arith_type(t);
        }
        c.builtin = name == "min" ? Builtin::Min : Builtin::Max;
        if (c.args.size() < 2) throw SemaError(e.loc, name + "() needs at least two arguments");
        for (auto& arg : c.args) {
            const Type* t = arg->type;
            if (t->is_error()) return t;
            if (!t->is_numeric()) throw SemaError(arg->loc, name + "() needs numbers, found " + t->str());
        }
        // Arguments of one type keep it (min of two chars is a char); integer literals adapt to
        // that type when they fit. Mixed types follow the arithmetic rules (promotion to int,
        // wider wins, float wins), so min/max always agree with `<`.
        const Type* same = nullptr;
        bool uniform = true;
        for (auto& arg : c.args) {
            if (is_int_literal(*arg)) continue;
            if (!same) same = arg->type;
            else if (same != arg->type) uniform = false;
        }
        if (uniform && same) {
            for (auto& arg : c.args) {
                if (is_int_literal(*arg) && !(same->is_integral() ? literal_fits(*arg, same) : true)) uniform = false;
            }
        }
        const Type* result = nullptr;
        if (uniform && same) {
            result = same;
        } else {
            for (auto& arg : c.args) {
                const Type* t = arg->type;
                if (!result) {
                    result = t->kind == TypeKind::Float ? t : arith_type(t);
                } else if (result->kind == TypeKind::Float || t->kind == TypeKind::Float) {
                    result = types.float_t;
                } else {
                    const Type* a = arith_type(result);
                    const Type* b = arith_type(t);
                    result = a == b ? a : a->bits != b->bits ? (a->bits > b->bits ? a : b) : (a->is_signed ? b : a);
                }
            }
        }
        for (auto& arg : c.args) {
            // like comparisons: a literal that does not fit the result type is converted, not rejected
            if (is_int_literal(*arg) && result->is_integral() && !literal_fits(*arg, result)) continue;
            coerce(*arg, result, "in " + name + "()");
        }
        return result;
    }

    if (const auto it = functions.find(name); it != functions.end()) {
        FnInfo* fn = it->second;
        c.kind = CallKind::Function;
        c.fn = fn;
        if (c.args.size() != fn->params.size()) {
            throw SemaError(e.loc, "function '" + name + "' expects " + std::to_string(fn->params.size())
                                   + " argument(s), got " + std::to_string(c.args.size()));
        }
        std::vector<std::pair<PlacePath, SourceLoc>> refs;
        for (size_t i = 0; i < c.args.size(); i++) {
            Expr& arg = *c.args[i];
            const VarInfo* param = fn->params[i];
            if (param->is_ref) {
                check_expr(arg);
                require_place(arg, "pass to reference parameter '" + param->name + "'");
                if (arg.type != param->type && !arg.type->is_error() && !param->type->is_error()) {
                    throw SemaError(arg.loc, "reference parameter '" + param->name + "' of '" + name
                                             + "' needs a variable of type " + param->type->str() + ", found "
                                             + arg.type->str());
                }
                const PlacePath path = place_path(arg);
                if (path.through_map) note_write(arg); // taking m[k] by reference inserts k
                if (path.root) {
                    ref_args.push_back({current_fn, fn, i, path.root, arg.loc, path.through_element});
                    refs.emplace_back(path, arg.loc);
                }
            } else {
                check_expr(arg, param->type);
                coerce(arg, param->type, "for parameter '" + param->name + "' of '" + name + "'");
            }
        }
        check_ref_aliasing(refs, name);
        if (current_fn) current_fn->callees.insert(fn);
        return fn->ret;
    }

    if (const auto it = structs.find(name); it != structs.end()) {
        StructInfo* st = it->second;
        c.kind = CallKind::StructCtor;
        c.st = st;
        if (!c.args.empty() && c.args.size() != st->fields.size()) {
            throw SemaError(e.loc, "struct '" + name + "' has " + std::to_string(st->fields.size())
                                   + " field(s); construct it with no arguments or with all of them");
        }
        for (size_t i = 0; i < c.args.size(); i++) {
            check_expr(*c.args[i], st->fields[i].type);
            coerce(*c.args[i], st->fields[i].type, "for field '" + st->fields[i].name + "' of '" + name + "'");
        }
        return st->type;
    }

    if (lookup_var(name)) throw SemaError(c.callee_loc, "'" + name + "' is a variable, not a function");
    throw SemaError(c.callee_loc, "unknown function '" + name + "'");
}

const Type* SemanticAnalyzer::check_cast(Expr& e, CastExpr& c) {
    const Type* target = resolve_base_type(c.target);
    const Type* from = check_expr(*c.operand);
    if (from->is_error()) return target;
    if (from->kind == TypeKind::Void) throw SemaError(e.loc, "cannot convert a value-less call");
    bool ok = false;
    if (target->is_integral()) ok = from->is_numeric() || from->kind == TypeKind::Bool;
    else if (target->kind == TypeKind::Float) ok = from->is_numeric();
    else if (target->kind == TypeKind::Bool) ok = from->is_integral() || from->kind == TypeKind::Bool;
    else if (target->kind == TypeKind::String) ok = from->is_scalar() || from->kind == TypeKind::String;
    if (!ok) throw SemaError(e.loc, "cannot convert " + from->str() + " to " + target->str());
    return target;
}

void SemanticAnalyzer::check_integral_index(Expr& index) {
    const Type* t = check_expr(index);
    if (!t->is_integral() && !t->is_error()) {
        throw SemaError(index.loc, "index must be an integer, found " + t->str());
    }
}

const Type* SemanticAnalyzer::check_index(Expr& e, IndexExpr& ix) {
    const Type* bt = check_expr(*ix.base);
    if (bt->is_error()) {
        check_expr(*ix.index);
        return types.error_t;
    }
    if (bt->is_sequence()) {
        check_integral_index(*ix.index);
        return bt->element();
    }
    if (bt->kind == TypeKind::Map) {
        check_expr(*ix.index, bt->key);
        coerce(*ix.index, bt->key, "as map key");
        return bt->val;
    }
    throw SemaError(e.loc, "cannot index a value of type " + bt->str());
}

const Type* SemanticAnalyzer::check_method(Expr& e, MethodCallExpr& m) {
    const Type* bt = check_expr(*m.base);
    if (bt->is_error()) {
        for (auto& a : m.args) check_expr(*a);
        return types.error_t;
    }
    const auto arity = [&](const size_t n) {
        if (m.args.size() != n) {
            throw SemaError(e.loc, "." + m.method + "() takes " + std::to_string(n) + " argument(s), got "
                                   + std::to_string(m.args.size()));
        }
    };
    const auto unknown = [&]() -> SemaError {
        return SemaError(e.loc, "type " + bt->str() + " has no method '" + m.method + "'");
    };
    const bool is_vec = bt->kind == TypeKind::Vec;
    const bool is_str = bt->kind == TypeKind::String;
    const bool is_map = bt->kind == TypeKind::Map;

    if (m.method == "len") {
        if (!bt->is_sequence() && !is_map) throw unknown();
        arity(0);
        m.resolved = Method::Len;
        return types.int_t;
    }
    if (m.method == "push") {
        if (bt->kind == TypeKind::Arr) {
            throw SemaError(e.loc, "cannot push to a fixed-size array (" + bt->str() + "); use vec<" + bt->elem->str()
                                   + ">");
        }
        if (!is_vec && !is_str) throw unknown();
        arity(1);
        require_place(*m.base, "push to");
        check_expr(*m.args[0], bt->element());
        coerce(*m.args[0], bt->element(), "in push()");
        note_write(*m.base);
        m.resolved = Method::Push;
        return types.void_t;
    }
    if (m.method == "pop") {
        if (!is_vec && !is_str) throw unknown();
        arity(0);
        require_place(*m.base, "pop from");
        note_write(*m.base);
        m.resolved = Method::Pop;
        return bt->element();
    }
    if (m.method == "back") {
        if (!bt->is_sequence()) throw unknown();
        arity(0);
        m.resolved = Method::Back;
        return bt->element();
    }
    if (m.method == "clear") {
        if (!is_vec && !is_str && !is_map) throw unknown();
        arity(0);
        require_place(*m.base, "clear");
        note_write(*m.base);
        m.resolved = Method::Clear;
        return types.void_t;
    }
    if (m.method == "has" || m.method == "erase") {
        if (!is_map) throw unknown();
        arity(1);
        check_expr(*m.args[0], bt->key);
        coerce(*m.args[0], bt->key, "as map key");
        if (m.method == "erase") {
            require_place(*m.base, "erase from");
            note_write(*m.base);
            m.resolved = Method::Erase;
            return types.void_t;
        }
        m.resolved = Method::Has;
        return types.bool_t;
    }
    if (m.method == "sort") {
        if (!bt->is_sequence()) throw unknown();
        arity(0);
        const Type* el = bt->element();
        const bool sortable = (el->is_numeric() && el->bits <= 128) || el->kind == TypeKind::String
                              || el->kind == TypeKind::Bool;
        if (!sortable) {
            throw SemaError(e.loc, "sort() supports numbers up to 128 bits, chars, bools and strings, not "
                                   + el->str());
        }
        require_place(*m.base, "sort");
        note_write(*m.base);
        m.resolved = Method::Sort;
        return types.void_t;
    }
    throw unknown();
}

/*** statements ***/

void SemanticAnalyzer::check_condition(Expr& cond, const std::string& construct) {
    const Type* t = check_expr(cond);
    if (t->kind != TypeKind::Bool && !t->is_error()) {
        throw SemaError(cond.loc, "condition of '" + construct + "' must be bool, found " + t->str());
    }
}

void SemanticAnalyzer::check_scoped(Stmt& s) {
    push_scope();
    guard([&] { check_stmt(s); });
    pop_scope();
}

void SemanticAnalyzer::check_var_decl(Stmt& s, VarDeclStmt& d) {
    const Type* t = types.error_t;
    guard([&] { t = resolve_type(d.type, true); });
    d.resolved = t;
    for (auto& dim : d.type.dims) {
        if (dim) guard([&] { check_integral_index(*dim); });
    }
    const bool has_sizes = !d.type.dims.empty() && d.type.dims[0];
    if (d.from_read) {
        guard([&] {
            if (t->kind == TypeKind::Arr && !has_sizes) {
                throw SemaError(s.loc, "reading an array needs a size, e.g. read int[n] a;");
            }
            if (!readable(t)) throw SemaError(s.loc, "cannot read a value of type " + t->str());
        });
        note_io(s.loc);
    }
    for (auto& decl : d.decls) {
        if (decl.init) {
            guard([&] {
                if (has_sizes) {
                    throw SemaError(decl.init->loc, "cannot combine an array size with an initializer");
                }
                check_expr(*decl.init, t);
                coerce(*decl.init, t, "in initialization of '" + decl.name + "'");
            });
        }
        guard([&] { decl.var = declare_var(decl.name, t, decl.loc); });
    }
}

void SemanticAnalyzer::check_fn(Stmt& s, FnDecl& f) {
    if (!f.info || !at_global_scope()) {
        throw SemaError(s.loc, "functions can only be declared at the top level");
    }
    FnInfo* fn = f.info;
    FnInfo* saved_fn = current_fn;
    const int saved_loop = loop_depth;
    current_fn = fn;
    loop_depth = 0;
    push_scope();
    for (auto& p : f.params) {
        guard([&] {
            if (scopes.back().contains(p.name)) throw SemaError(p.loc, "duplicate parameter '" + p.name + "'");
            if (structs.contains(p.name)) {
                throw SemaError(p.loc, "parameter '" + p.name + "' has the same name as a struct");
            }
            scopes.back()[p.name] = p.var;
        });
    }
    guard([&] { check_stmt(*f.body); });
    if (fn->ret->kind != TypeKind::Void && !fn->ret->is_error() && !always_returns(*f.body)) {
        report(SemaError(s.loc, "function '" + f.name + "' can reach the end without returning a value"));
    }
    pop_scope();
    current_fn = saved_fn;
    loop_depth = saved_loop;
}

bool SemanticAnalyzer::contains_break(const Stmt& s) {
    return std::visit(overloaded{
        [](const BreakStmt&) { return true; },
        [](const BlockStmt& b) {
            for (const auto& c : b.stmts) {
                if (contains_break(*c)) return true;
            }
            return false;
        },
        [](const IfStmt& i) {
            return contains_break(*i.then_branch) || (i.else_branch && contains_break(*i.else_branch));
        },
        [](const auto&) { return false; }, // nested loops own their breaks
    }, s.node);
}

bool SemanticAnalyzer::always_returns(const Stmt& s) {
    return std::visit(overloaded{
        [](const ReturnStmt&) { return true; },
        [](const BlockStmt& b) {
            for (const auto& c : b.stmts) {
                if (always_returns(*c)) return true;
            }
            return false;
        },
        [](const IfStmt& i) {
            return i.else_branch && always_returns(*i.then_branch) && always_returns(*i.else_branch);
        },
        [](const ExprStmt& x) {
            const auto* call = std::get_if<CallExpr>(&x.expr->node);
            return call && call->builtin == Builtin::Exit;
        },
        [](const WhileStmt& w) {
            const auto* lit = std::get_if<BoolLit>(&w.cond->node);
            return lit && lit->value && !contains_break(*w.body);
        },
        [](const ForCStmt& f) { return !f.cond && !contains_break(*f.body); },
        [](const auto&) { return false; },
    }, s.node);
}

void SemanticAnalyzer::check_stmt(Stmt& s) {
    std::visit(overloaded{
        [&](ExprStmt& x) { check_expr(*x.expr); },
        [&](BlockStmt& b) {
            push_scope();
            for (auto& child : b.stmts) guard([&] { check_stmt(*child); });
            pop_scope();
        },
        [&](VarDeclStmt& d) { check_var_decl(s, d); },
        [&](ReadStmt& r) {
            for (auto& target : r.targets) {
                guard([&] {
                    const Type* t = check_expr(*target);
                    require_place(*target, "read into");
                    if (!readable(t)) throw SemaError(target->loc, "cannot read a value of type " + t->str());
                    note_write(*target);
                });
            }
            note_io(s.loc);
        },
        [&](PrintStmt& p) {
            for (auto& arg : p.args) {
                guard([&] {
                    const Type* t = check_expr(*arg);
                    if (t->kind == TypeKind::Void) {
                        throw SemaError(arg->loc, "cannot print the result of a function that returns nothing");
                    }
                    if (!printable(t)) throw SemaError(arg->loc, "cannot print a value of type " + t->str());
                });
            }
            note_io(s.loc);
        },
        [&](IfStmt& i) {
            guard([&] { check_condition(*i.cond, "if"); });
            check_scoped(*i.then_branch);
            if (i.else_branch) check_scoped(*i.else_branch);
        },
        [&](WhileStmt& w) {
            guard([&] { check_condition(*w.cond, "while"); });
            loop_depth++;
            check_scoped(*w.body);
            loop_depth--;
        },
        [&](ForInStmt& f) {
            push_scope();
            const Type* var_type = types.error_t;
            guard([&] {
                if (auto* range = std::get_if<RangeLit>(&f.iterable->node)) {
                    check_expr(*f.iterable); // types bounds, yields vec<T>
                    (void)range;
                    var_type = f.iterable->type->is_error() ? types.error_t : f.iterable->type->elem;
                    return;
                }
                const Type* it = check_expr(*f.iterable);
                if (it->is_error()) return;
                if (it->is_sequence()) var_type = it->element();
                else if (it->kind == TypeKind::Map) var_type = it->key;
                else throw SemaError(f.iterable->loc, "cannot iterate over a value of type " + it->str());
            });
            guard([&] { f.var = declare_var(f.var_name, var_type, f.var_loc); });
            loop_depth++;
            check_scoped(*f.body);
            loop_depth--;
            pop_scope();
        },
        [&](ForCStmt& f) {
            push_scope();
            if (f.init) guard([&] { check_stmt(*f.init); });
            if (f.cond) guard([&] { check_condition(*f.cond, "for"); });
            if (f.step) guard([&] { check_expr(*f.step); });
            loop_depth++;
            check_scoped(*f.body);
            loop_depth--;
            pop_scope();
        },
        [&](TCStmt& t) {
            if (current_fn) throw SemaError(s.loc, "TC blocks cannot appear inside functions");
            if (tc_depth > 0) throw SemaError(s.loc, "TC blocks cannot be nested");
            info.has_tc = true;
            tc_depth++;
            loop_depth++;
            check_scoped(*t.body);
            loop_depth--;
            tc_depth--;
        },
        [&](ReturnStmt& r) {
            if (!current_fn) throw SemaError(s.loc, "'return' outside of a function");
            const Type* ret = current_fn->ret;
            if (r.value) {
                if (ret->kind == TypeKind::Void) {
                    check_expr(*r.value);
                    throw SemaError(r.value->loc, "function '" + current_fn->name + "' does not return a value");
                }
                check_expr(*r.value, ret);
                coerce(*r.value, ret, "in return statement");
            } else if (ret->kind != TypeKind::Void && !ret->is_error()) {
                throw SemaError(s.loc, "function '" + current_fn->name + "' must return a value of type "
                                       + ret->str());
            }
        },
        [&](BreakStmt&) {
            if (loop_depth == 0) throw SemaError(s.loc, "'break' outside of a loop");
        },
        [&](ContinueStmt&) {
            if (loop_depth == 0) throw SemaError(s.loc, "'continue' outside of a loop");
        },
        [&](FnDecl& f) { check_fn(s, f); },
        [&](StructDecl& st) {
            if (!st.info || !at_global_scope()) {
                throw SemaError(s.loc, "structs can only be declared at the top level");
            }
        },
    }, s.node);
}
