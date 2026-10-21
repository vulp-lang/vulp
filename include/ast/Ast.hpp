#ifndef VULP_AST_HPP
#define VULP_AST_HPP

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/StringRef.h>

#include "lexer/TokenType.hpp"
#include "misc/Diagnostics.hpp"

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// Semantic information attached to the AST by the semantic analyser.
namespace Sema {
    struct Type;
    struct VarInfo;
    struct FnInfo;
    struct StructInfo;
}

namespace AST {
    struct Expr;
    struct Stmt;
    using ExprPtr = std::unique_ptr<Expr>;
    using StmtPtr = std::unique_ptr<Stmt>;

    /*** Types as written in the source ***/

    struct TypeSpec {
        enum class Kind {
            Int,      // int (= i64)
            IntN,     // iN / uN, any 1 <= N <= 256 (rounded up to a power of two by sema)
            RangeInt, // int<lo..hi> (width chosen by sema)
            Char,
            Bool,
            Float,
            String,
            Vec,      // vec<T>
            Map,      // map<K, V>
            Named,    // struct name
        };

        Kind kind = Kind::Int;
        SourceLoc loc;
        int bits = 0;              // IntN
        bool is_signed = true;     // IntN
        std::string lo, hi;        // RangeInt bounds as decimal text (may start with '-')
        std::string name;          // Named
        std::vector<TypeSpec> args; // Vec: [elem], Map: [key, value]
        // Array suffixes, outermost first: `int[n][m]` -> dims = {n, m}. A nullptr entry is
        // an unsized dimension `[]`.
        std::vector<ExprPtr> dims;
        bool is_ref = false;       // `T&` (function parameters only)
    };

    /*** Expressions ***/

    struct IntLit {
        std::string text;   // decimal digits or "0x..." (separators removed)
        llvm::APInt value;  // filled by sema
    };
    struct FloatLit { std::string text; };
    struct CharLit { unsigned char value; };
    struct StringLit { std::string value; };
    struct BoolLit { bool value; };

    struct ArrayLit { std::vector<ExprPtr> elems; };   // [a, b, c]
    struct RangeLit { ExprPtr lo, hi; };                // [lo..hi], inclusive

    struct NameExpr {
        std::string name;
        Sema::VarInfo* var = nullptr; // resolved by sema
    };

    struct UnaryExpr {
        TokenType op; // MINUS, NOT, TILDE
        ExprPtr operand;
    };

    struct BinaryExpr {
        TokenType op;
        ExprPtr lhs, rhs;
        const Sema::Type* operand_type = nullptr; // common type both operands are converted to
    };

    struct AssignExpr {
        TokenType op; // EQUAL or a compound assignment
        ExprPtr target, value;
        const Sema::Type* operand_type = nullptr; // for compound assignment: type the operation runs in
    };

    struct IncDecExpr { // postfix x++ / x--
        TokenType op;
        ExprPtr target;
    };

    enum class CallKind { Unresolved, Function, StructCtor, Builtin };
    enum class Builtin { None, Min, Max, Abs, Exit };

    struct CallExpr {
        std::string callee;
        SourceLoc callee_loc;
        std::vector<ExprPtr> args;
        // filled by sema
        CallKind kind = CallKind::Unresolved;
        Builtin builtin = Builtin::None;
        Sema::FnInfo* fn = nullptr;
        Sema::StructInfo* st = nullptr;
    };

    struct CastExpr { // int(x), i32(x), char(x), float(x), bool(x), string(x)
        TypeSpec target;
        ExprPtr operand;
    };

    struct IndexExpr { ExprPtr base, index; };

    struct FieldExpr {
        ExprPtr base;
        std::string field;
        int field_index = -1; // resolved by sema
    };

    enum class Method { None, Len, Push, Pop, Back, Clear, Has, Erase, Sort };

    struct MethodCallExpr {
        ExprPtr base;
        std::string method;
        std::vector<ExprPtr> args;
        Method resolved = Method::None;
    };

    using ExprNode = std::variant<IntLit, FloatLit, CharLit, StringLit, BoolLit, ArrayLit, RangeLit, NameExpr,
                                  UnaryExpr, BinaryExpr, AssignExpr, IncDecExpr, CallExpr, CastExpr, IndexExpr,
                                  FieldExpr, MethodCallExpr>;

    struct Expr {
        ExprNode node;
        SourceLoc loc;
        const Sema::Type* type = nullptr; // filled by sema
    };

    template<class T>
    ExprPtr make_expr(T&& node, const SourceLoc loc) {
        auto e = std::make_unique<Expr>(Expr{std::forward<T>(node), loc, nullptr});
        return e;
    }

    /*** Statements ***/

    struct ExprStmt { ExprPtr expr; };

    struct BlockStmt { std::vector<StmtPtr> stmts; };

    struct Declarator {
        std::string name;
        SourceLoc loc;
        ExprPtr init;                 // may be null
        Sema::VarInfo* var = nullptr; // filled by sema
    };

    // `T a = 1, b;`  or, when `from_read` is set, `read T a, b;`
    struct VarDeclStmt {
        TypeSpec type;
        std::vector<Declarator> decls;
        bool from_read = false;
        const Sema::Type* resolved = nullptr;
    };

    struct ReadStmt { std::vector<ExprPtr> targets; };

    struct PrintStmt {
        std::vector<ExprPtr> args;
        bool newline = false;
    };

    struct IfStmt {
        ExprPtr cond;
        StmtPtr then_branch;
        StmtPtr else_branch; // may be null; `elif` is lowered to a nested IfStmt
    };

    struct WhileStmt {
        ExprPtr cond;
        StmtPtr body;
    };

    struct ForInStmt {
        std::string var_name;
        SourceLoc var_loc;
        ExprPtr iterable;
        StmtPtr body;
        Sema::VarInfo* var = nullptr;
    };

    struct ForCStmt { // for (init; cond; step): body
        StmtPtr init;  // VarDeclStmt or ExprStmt, may be null
        ExprPtr cond;  // may be null (= true)
        ExprPtr step;  // may be null
        StmtPtr body;
    };

    struct TCStmt { StmtPtr body; };

    struct ReturnStmt { ExprPtr value; }; // value may be null

    struct BreakStmt {};
    struct ContinueStmt {};

    struct Param {
        TypeSpec type;
        std::string name;
        SourceLoc loc;
        Sema::VarInfo* var = nullptr;
    };

    struct FnDecl {
        bool is_dp = false;
        std::string name;
        std::vector<Param> params;
        std::optional<TypeSpec> ret; // empty -> returns nothing
        StmtPtr body;                // BlockStmt
        Sema::FnInfo* info = nullptr;
    };

    struct StructField {
        TypeSpec type;
        std::string name;
        SourceLoc loc;
    };

    struct StructDecl {
        std::string name;
        std::vector<StructField> fields;
        Sema::StructInfo* info = nullptr;
    };

    using StmtNode = std::variant<ExprStmt, BlockStmt, VarDeclStmt, ReadStmt, PrintStmt, IfStmt, WhileStmt, ForInStmt,
                                  ForCStmt, TCStmt, ReturnStmt, BreakStmt, ContinueStmt, FnDecl, StructDecl>;

    struct Stmt {
        StmtNode node;
        SourceLoc loc;
    };

    template<class T>
    StmtPtr make_stmt(T&& node, const SourceLoc loc) {
        return std::make_unique<Stmt>(Stmt{std::forward<T>(node), loc});
    }

    struct Program {
        std::vector<StmtPtr> items;
    };

    // Copies a TypeSpec whose array dimensions are all unsized (`[]`). Sized dimensions own
    // expressions and are never cloned.
    TypeSpec clone_type_spec(const TypeSpec& src);

    /*** Printing (S-expressions; used by --ast and the parser tests) ***/

    std::string print_type_spec(const TypeSpec& type);
    std::string print_expr(const Expr& expr);
    std::string print_stmt(const Stmt& stmt);
    std::string print_program(const Program& program);
    // Same as print_program but annotates every expression with its semantic type: `x:i64`.
    std::string print_program_typed(const Program& program);
} // namespace AST

#endif // VULP_AST_HPP
