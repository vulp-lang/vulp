#include "ast/Ast.hpp"
#include "sema/Types.hpp"

namespace AST {
    namespace {
        std::string escape(const std::string& s) {
            std::string out;
            for (const char c : s) {
                switch (c) {
                    case '\n': out += "\\n"; break;
                    case '\t': out += "\\t"; break;
                    case '\r': out += "\\r"; break;
                    case '\0': out += "\\0"; break;
                    case '\\': out += "\\\\"; break;
                    case '"': out += "\\\""; break;
                    case '\'': out += "\\'"; break;
                    default: out += c;
                }
            }
            return out;
        }

        class Printer {
        public:
            bool typed = false;

            std::string type_spec(const TypeSpec& t) {
                std::string out;
                switch (t.kind) {
                    case TypeSpec::Kind::Int: out = "int"; break;
                    case TypeSpec::Kind::IntN: out = (t.is_signed ? "i" : "u") + std::to_string(t.bits); break;
                    case TypeSpec::Kind::RangeInt: out = "int<" + t.lo + ".." + t.hi + ">"; break;
                    case TypeSpec::Kind::Char: out = "char"; break;
                    case TypeSpec::Kind::Bool: out = "bool"; break;
                    case TypeSpec::Kind::Float: out = "float"; break;
                    case TypeSpec::Kind::String: out = "string"; break;
                    case TypeSpec::Kind::Vec: out = "vec<" + type_spec(t.args[0]) + ">"; break;
                    case TypeSpec::Kind::Map:
                        out = "map<" + type_spec(t.args[0]) + "," + type_spec(t.args[1]) + ">";
                        break;
                    case TypeSpec::Kind::Named: out = t.name; break;
                }
                for (const auto& d : t.dims) out += "[" + (d ? expr(*d) : "") + "]";
                if (t.is_ref) out += "&";
                return out;
            }

            std::string list(const std::vector<ExprPtr>& exprs) {
                std::string out;
                for (const auto& e : exprs) out += " " + expr(*e);
                return out;
            }

            std::string expr(const Expr& e) {
                std::string body = std::visit(overloaded{
                    [&](const IntLit& x) -> std::string { return x.text; },
                    [&](const FloatLit& x) -> std::string { return x.text; },
                    [&](const CharLit& x) -> std::string {
                        return "'" + escape(std::string(1, static_cast<char>(x.value))) + "'";
                    },
                    [&](const StringLit& x) -> std::string { return "\"" + escape(x.value) + "\""; },
                    [&](const BoolLit& x) -> std::string { return x.value ? "true" : "false"; },
                    [&](const ArrayLit& x) -> std::string { return "(array" + list(x.elems) + ")"; },
                    [&](const RangeLit& x) -> std::string {
                        return "(range " + expr(*x.lo) + " " + expr(*x.hi) + ")";
                    },
                    [&](const NameExpr& x) -> std::string { return x.name; },
                    [&](const UnaryExpr& x) -> std::string {
                        return std::string("(") + token_type_to_string(x.op) + " " + expr(*x.operand) + ")";
                    },
                    [&](const BinaryExpr& x) -> std::string {
                        return std::string("(") + token_type_to_string(x.op) + " " + expr(*x.lhs) + " "
                               + expr(*x.rhs) + ")";
                    },
                    [&](const AssignExpr& x) -> std::string {
                        return std::string("(") + token_type_to_string(x.op) + " " + expr(*x.target) + " "
                               + expr(*x.value) + ")";
                    },
                    [&](const IncDecExpr& x) -> std::string {
                        return std::string("(post") + token_type_to_string(x.op) + " " + expr(*x.target) + ")";
                    },
                    [&](const CallExpr& x) -> std::string { return "(call " + x.callee + list(x.args) + ")"; },
                    [&](const CastExpr& x) -> std::string {
                        return "(cast " + type_spec(x.target) + " " + expr(*x.operand) + ")";
                    },
                    [&](const IndexExpr& x) -> std::string {
                        return "(index " + expr(*x.base) + " " + expr(*x.index) + ")";
                    },
                    [&](const FieldExpr& x) -> std::string { return "(. " + expr(*x.base) + " " + x.field + ")"; },
                    [&](const MethodCallExpr& x) -> std::string {
                        return "(method " + expr(*x.base) + " " + x.method + list(x.args) + ")";
                    },
                }, e.node);
                if (typed) body += ":" + (e.type ? e.type->str() : std::string("?"));
                return body;
            }

            std::string opt_stmt(const StmtPtr& s) { return s ? stmt(*s) : "_"; }
            std::string opt_expr(const ExprPtr& e) { return e ? expr(*e) : "_"; }

            std::string stmt(const Stmt& s) {
                return std::visit(overloaded{
                    [&](const ExprStmt& x) -> std::string { return expr(*x.expr); },
                    [&](const BlockStmt& x) -> std::string {
                        std::string out = "(block";
                        for (const auto& child : x.stmts) out += " " + stmt(*child);
                        return out + ")";
                    },
                    [&](const VarDeclStmt& x) -> std::string {
                        std::string out = std::string(x.from_read ? "(read-var " : "(var ") + type_spec(x.type);
                        for (const auto& d : x.decls) {
                            out += " (" + d.name;
                            if (d.init) out += " " + expr(*d.init);
                            out += ")";
                        }
                        return out + ")";
                    },
                    [&](const ReadStmt& x) -> std::string { return "(read" + list(x.targets) + ")"; },
                    [&](const PrintStmt& x) -> std::string {
                        return std::string(x.newline ? "(println" : "(print") + list(x.args) + ")";
                    },
                    [&](const IfStmt& x) -> std::string {
                        std::string out = "(if " + expr(*x.cond) + " " + stmt(*x.then_branch);
                        if (x.else_branch) out += " " + stmt(*x.else_branch);
                        return out + ")";
                    },
                    [&](const WhileStmt& x) -> std::string {
                        return "(while " + expr(*x.cond) + " " + stmt(*x.body) + ")";
                    },
                    [&](const ForInStmt& x) -> std::string {
                        return "(for-in " + x.var_name + " " + expr(*x.iterable) + " " + stmt(*x.body) + ")";
                    },
                    [&](const ForCStmt& x) -> std::string {
                        return "(for " + opt_stmt(x.init) + " " + opt_expr(x.cond) + " " + opt_expr(x.step) + " "
                               + stmt(*x.body) + ")";
                    },
                    [&](const TCStmt& x) -> std::string { return "(TC " + stmt(*x.body) + ")"; },
                    [&](const ReturnStmt& x) -> std::string {
                        return x.value ? "(return " + expr(*x.value) + ")" : "(return)";
                    },
                    [&](const BreakStmt&) -> std::string { return "(break)"; },
                    [&](const ContinueStmt&) -> std::string { return "(continue)"; },
                    [&](const FnDecl& x) -> std::string {
                        std::string out = std::string(x.is_dp ? "(dp-fn " : "(fn ") + x.name + " (";
                        for (size_t i = 0; i < x.params.size(); i++) {
                            if (i) out += " ";
                            out += "(" + type_spec(x.params[i].type) + " " + x.params[i].name + ")";
                        }
                        out += ")";
                        if (x.ret) out += " -> " + type_spec(*x.ret);
                        return out + " " + stmt(*x.body) + ")";
                    },
                    [&](const StructDecl& x) -> std::string {
                        std::string out = "(struct " + x.name;
                        for (const auto& f : x.fields) out += " (" + type_spec(f.type) + " " + f.name + ")";
                        return out + ")";
                    },
                }, s.node);
            }
        };
    } // namespace

    TypeSpec clone_type_spec(const TypeSpec& src) {
        TypeSpec out;
        out.kind = src.kind;
        out.loc = src.loc;
        out.bits = src.bits;
        out.is_signed = src.is_signed;
        out.lo = src.lo;
        out.hi = src.hi;
        out.name = src.name;
        for (const auto& a : src.args) out.args.push_back(clone_type_spec(a));
        for (size_t i = 0; i < src.dims.size(); i++) out.dims.push_back(nullptr);
        out.is_ref = src.is_ref;
        return out;
    }

    std::string print_type_spec(const TypeSpec& type) {
        Printer p;
        return p.type_spec(type);
    }

    std::string print_expr(const Expr& expr) {
        Printer p;
        return p.expr(expr);
    }

    std::string print_stmt(const Stmt& stmt) {
        Printer p;
        return p.stmt(stmt);
    }

    std::string print_program(const Program& program) {
        Printer p;
        std::string out;
        for (const auto& item : program.items) out += p.stmt(*item) + "\n";
        return out;
    }

    std::string print_program_typed(const Program& program) {
        Printer p;
        p.typed = true;
        std::string out;
        for (const auto& item : program.items) out += p.stmt(*item) + "\n";
        return out;
    }
} // namespace AST
