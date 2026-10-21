#ifndef VULP_SEMANTICS_HPP
#define VULP_SEMANTICS_HPP

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/Ast.hpp"
#include "misc/Diagnostics.hpp"
#include "sema/Types.hpp"

class SemaError : public std::runtime_error {
public:
    SourceLoc loc;
    SemaError(const SourceLoc l, const std::string& msg) : std::runtime_error(msg), loc(l) {}
};

// Resolves names and types, annotates the AST (Expr::type, NameExpr::var, CallExpr::fn, ...)
// and checks the language rules. Errors are collected in `diag`; analysis continues with the
// next statement after an error.
class SemanticAnalyzer {
private:
    AST::Program& program;
    Diagnostics& diag;
    Sema::TypeContext& types;
    Sema::ProgramInfo info;

    std::vector<std::unordered_map<std::string, Sema::VarInfo*>> scopes;
    std::unordered_map<std::string, Sema::FnInfo*> functions;
    std::unordered_map<std::string, Sema::StructInfo*> structs;

    // A variable passed to a `T&` parameter. Whether that counts as a write is only known once
    // every function body has been analysed (see propagate_purity).
    struct RefArg {
        Sema::FnInfo* caller;
        Sema::FnInfo* callee;
        size_t param_index;
        Sema::VarInfo* root;
        SourceLoc loc;
        bool through_element; // the argument is (inside) an element of a container: a[i], m[k].f
    };
    // The shape of a place expression: its root variable and the steps from it (a field index,
    // or -1 for an element access).
    struct PlacePath {
        Sema::VarInfo* root = nullptr;
        std::vector<int> steps;
        std::vector<const Sema::Type*> containers; // the container types whose elements the path enters
        const Sema::Type* type = nullptr;          // type of the whole place
        bool through_element = false;
        bool through_map = false;
    };
    [[nodiscard]] bool may_alias(const Sema::VarInfo* a, const Sema::VarInfo* b) const;
    static PlacePath place_path(const AST::Expr& e);
    void check_ref_aliasing(const std::vector<std::pair<PlacePath, SourceLoc>>& refs, const std::string& callee);
    std::vector<RefArg> ref_args;

    Sema::FnInfo* current_fn = nullptr;
    int loop_depth = 0;
    int tc_depth = 0;
    int error_count = 0;

    // errors
    void report(const SemaError& e);
    template<class F> void guard(F&& fn);

    // registration
    void register_structs();
    void resolve_struct_fields();
    void register_functions();
    void propagate_purity();

    // scopes & symbols
    void push_scope() { scopes.emplace_back(); }
    void pop_scope() { scopes.pop_back(); }
    [[nodiscard]] bool at_global_scope() const { return scopes.size() == 1 && current_fn == nullptr; }
    Sema::VarInfo* declare_var(const std::string& name, const Sema::Type* type, SourceLoc loc);
    Sema::VarInfo* lookup_var(const std::string& name) const;

    // types
    const Sema::Type* resolve_type(AST::TypeSpec& spec, bool allow_sizes);
    const Sema::Type* resolve_base_type(const AST::TypeSpec& spec);
    [[nodiscard]] bool assignable(const Sema::Type* to, const Sema::Type* from) const;
    void coerce(AST::Expr& e, const Sema::Type* to, const std::string& context);
    [[nodiscard]] const Sema::Type* arith_type(const Sema::Type* t) const; // char promotes to int
    const Sema::Type* common_type(AST::Expr& lhs, AST::Expr& rhs, bool promote_char);
    void retype_literal(AST::Expr& e, const Sema::Type* to);
    [[nodiscard]] static bool is_int_literal(const AST::Expr& e);
    [[nodiscard]] static bool literal_fits(const AST::Expr& e, const Sema::Type* to);
    [[nodiscard]] bool printable(const Sema::Type* t) const;
    [[nodiscard]] bool readable(const Sema::Type* t) const;

    // purity bookkeeping
    static AST::Expr* place_root(AST::Expr& e);
    [[nodiscard]] static bool is_place(const AST::Expr& e);
    void note_write(AST::Expr& target);
    void note_io(SourceLoc loc);

    // expressions
    const Sema::Type* check_expr(AST::Expr& e, const Sema::Type* expected = nullptr);
    const Sema::Type* check_int_lit(AST::Expr& e, AST::IntLit& lit);
    const Sema::Type* check_array_lit(AST::Expr& e, AST::ArrayLit& lit, const Sema::Type* expected);
    const Sema::Type* check_range_lit(AST::Expr& e, AST::RangeLit& lit, const Sema::Type* expected);
    const Sema::Type* check_unary(AST::Expr& e, AST::UnaryExpr& u);
    const Sema::Type* check_binary(AST::Expr& e, AST::BinaryExpr& b);
    const Sema::Type* binary_result(AST::Expr& e, TokenType op, AST::Expr& lhs, AST::Expr& rhs,
                                    const Sema::Type*& operand_type);
    const Sema::Type* check_assign(AST::Expr& e, AST::AssignExpr& a);
    const Sema::Type* check_call(AST::Expr& e, AST::CallExpr& c);
    const Sema::Type* check_cast(AST::Expr& e, AST::CastExpr& c);
    const Sema::Type* check_index(AST::Expr& e, AST::IndexExpr& ix);
    const Sema::Type* check_method(AST::Expr& e, AST::MethodCallExpr& m);
    void check_integral_index(AST::Expr& index);
    void require_place(AST::Expr& e, const std::string& what);

    // statements
    void check_stmt(AST::Stmt& s);
    void check_scoped(AST::Stmt& s); // body of if/while/for: always gets its own scope
    void check_var_decl(AST::Stmt& s, AST::VarDeclStmt& d);
    void check_fn(AST::Stmt& s, AST::FnDecl& f);
    void check_condition(AST::Expr& cond, const std::string& construct);
    [[nodiscard]] static bool always_returns(const AST::Stmt& s);
    [[nodiscard]] static bool contains_break(const AST::Stmt& s);

public:
    SemanticAnalyzer(AST::Program& p_program, Diagnostics& p_diag, Sema::TypeContext& p_types);
    bool analyze(); // returns true when there were no errors
    [[nodiscard]] bool has_errors() const { return error_count > 0; }
    Sema::ProgramInfo& program_info() { return info; }
};

#endif // VULP_SEMANTICS_HPP
