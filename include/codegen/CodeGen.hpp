#ifndef VULP_CODEGEN_HPP
#define VULP_CODEGEN_HPP

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "ast/Ast.hpp"
#include "sema/Types.hpp"

class CodeGenError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct CodeGenOptions {
    bool runtime_checks = true; // array bounds and integer division by zero
    bool emit_c_main = false;   // AOT: add `int main()` that calls vulp_rt_start(vulp_main)
};

// Lowers a type-checked Vulp program to an LLVM module.
//
// Runtime representation:
//   iN -> iN, char -> i8, bool -> i1, float -> double
//   string / vec<T> / T[] -> %vulp.vec = { ptr data, i64 len, i64 cap }
//   map<K, V> -> ptr (NULL = empty)
//   struct S -> %S = { fields... }
// Top-level statements form `i32 vulp_main()`; top-level declarations are LLVM globals.
//
// Ownership: containers have value semantics, so every heap buffer has exactly one owner (a
// variable, an element, a field, a map value or a temporary). Copying a value out of a place
// deep-copies it ("fix" helpers); overwriting a value, re-running a declaration, or leaving a
// function frees the old value ("free" helpers). See docs/architecture.md.
class CodeGen {
public:
    CodeGen(AST::Program& p_program, Sema::ProgramInfo& p_info, Sema::TypeContext& p_types, llvm::LLVMContext& p_ctx,
            const llvm::DataLayout& p_layout, const std::string& target_triple, CodeGenOptions p_opts);

    // Throws CodeGenError on internal errors (the program has already been type-checked).
    std::unique_ptr<llvm::Module> generate();

private:
    AST::Program& program;
    Sema::ProgramInfo& info;
    Sema::TypeContext& types;
    llvm::LLVMContext& ctx;
    const llvm::DataLayout& layout;
    std::unique_ptr<llvm::Module> module;
    llvm::IRBuilder<> b;
    CodeGenOptions opts;

    llvm::StructType* vec_ty = nullptr;
    std::unordered_map<const Sema::StructInfo*, llvm::StructType*> struct_types;

    // storage of every variable (alloca, global, or incoming pointer for `&` parameters)
    std::unordered_map<const Sema::VarInfo*, llvm::Value*> var_storage;

    // per-function state
    llvm::Function* cur_fn = nullptr;
    const Sema::FnInfo* cur_info = nullptr;   // nullptr while emitting vulp_main
    llvm::AllocaInst* ret_slot = nullptr;     // returns store here and branch to exit_bb ...
    llvm::BasicBlock* exit_bb = nullptr;      // ... which frees owned locals and returns
    std::vector<std::pair<llvm::Value*, const Sema::Type*>> owned_locals;
    struct LoopTargets {
        llvm::BasicBlock* break_bb;
        llvm::BasicBlock* continue_bb;
    };
    std::vector<LoopTargets> loops;

    std::unordered_map<const Sema::FnInfo*, llvm::Function*> fn_map;   // callable entry (wrapper for dp)
    std::unordered_map<const Sema::FnInfo*, llvm::Function*> dp_impl;  // dp: the real body
    std::unordered_map<const Sema::FnInfo*, llvm::GlobalVariable*> dp_memo;

    std::unordered_map<const Sema::Type*, llvm::Function*> fix_fns, free_fns, print_fns, read_fns;
    std::unordered_map<std::string, llvm::Constant*> string_constants, string_headers;

    // A value used once and then dropped. `slot` is set when the value is an owned temporary
    // that must be freed after use.
    struct Operand {
        llvm::Value* value = nullptr;
        llvm::Value* slot = nullptr;
        const Sema::Type* type = nullptr;
    };

    /*** CodeGen.cpp: setup, types, runtime, functions ***/
    llvm::Type* lty(const Sema::Type* t);
    uint64_t size_of(const Sema::Type* t);
    llvm::Type* i8() { return b.getInt8Ty(); }
    llvm::Type* i32() { return b.getInt32Ty(); }
    llvm::Type* i64() { return b.getInt64Ty(); }
    llvm::Type* ptr() { return b.getPtrTy(); }
    llvm::ConstantInt* c32(int64_t v) { return b.getInt32(static_cast<uint32_t>(v)); }
    llvm::ConstantInt* c64(int64_t v) { return b.getInt64(static_cast<uint64_t>(v)); }

    llvm::FunctionCallee rt(const std::string& name);
    llvm::AllocaInst* entry_alloca(llvm::Type* t, const std::string& name = "");
    // An entry-block slot that holds the zero value from function entry on (needed for slots
    // whose old value is freed before every store).
    llvm::AllocaInst* zeroed_alloca(const Sema::Type* t, const std::string& name = "");
    llvm::Value* spill(llvm::Value* v, const std::string& name = "tmp");
    llvm::Constant* string_constant(const std::string& s);
    llvm::Constant* string_header(const std::string& s); // read-only %vulp.vec for a literal
    void declare_structs();
    void declare_globals();
    void declare_functions();
    void emit_function(const Sema::FnInfo* fn);
    void emit_dp_wrapper(const Sema::FnInfo* fn);
    void emit_main();
    void emit_c_main();
    // Starts a fresh block after a terminator so that (dead) code can still be emitted.
    void start_dead_block();
    bool block_terminated() { return b.GetInsertBlock()->getTerminator() != nullptr; }

    /*** CodeGenExpr.cpp ***/
    llvm::Value* emit_expr(AST::Expr& e);
    llvm::Value* emit_owned(AST::Expr& e);
    llvm::Value* emit_as(AST::Expr& e, const Sema::Type* to, bool owned);
    // Address of a place. With write == false, reading a missing map key yields a zero value
    // instead of inserting the key.
    llvm::Value* emit_place(AST::Expr& e, bool write = true);
    Operand emit_operand(AST::Expr& e);
    void release_operand(const Operand& op);
    static bool is_place(const AST::Expr& e);
    static bool is_borrowed(const AST::Expr& e);
    // The innermost base of an index / field chain: `v` in `v[i].f`, `f()` in `f()[0]`.
    static const AST::Expr& chain_root(const AST::Expr& e);
    // Evaluates an index / field chain whose root is not a variable (e.g. `f()[0].x`): computes
    // the address inside the root value, calls use(address, root_is_owned) and then frees the
    // root if it was an owned temporary.
    template<class F> llvm::Value* with_temp_place(AST::Expr& e, F&& use);
    static bool has_side_effects(const AST::Expr& e);
    // A place whose index / key values have been evaluated once; its address can be recomputed
    // later (e.g. on every loop iteration) without re-running those expressions.
    struct FrozenPlace {
        llvm::Value* root = nullptr;
        struct Step {
            enum class Kind { Index, Key, Field } kind;
            const Sema::Type* base_type;
            llvm::Value* index = nullptr; // Index: i64
            Operand key;                  // Key: runtime-ready map key
            unsigned field = 0;           // Field
            int line = 0;
        };
        std::vector<Step> steps;
    };
    FrozenPlace freeze_place(AST::Expr& e);
    // write == false: a missing map key reads as a zero value; true: it is inserted
    llvm::Value* frozen_address(const FrozenPlace& place, bool write = false);
    void release_frozen(const FrozenPlace& place);
    llvm::Value* element_of_temporary(AST::Expr& e);
    llvm::Value* emit_int_lit(AST::Expr& e, const AST::IntLit& lit);
    llvm::Value* emit_string_lit(const std::string& s);
    llvm::Value* emit_array_lit(AST::Expr& e, AST::ArrayLit& lit);
    llvm::Value* emit_range_lit(AST::Expr& e, AST::RangeLit& lit);
    llvm::Value* emit_unary(AST::Expr& e, AST::UnaryExpr& u);
    llvm::Value* emit_binary(AST::Expr& e, AST::BinaryExpr& bin);
    llvm::Value* emit_logical(AST::BinaryExpr& bin);
    llvm::Value* emit_binop(TokenType op, llvm::Value* l, llvm::Value* r, const Sema::Type* t, int line);
    llvm::Value* emit_compare(TokenType op, llvm::Value* l, llvm::Value* r, const Sema::Type* t);
    llvm::Value* emit_string_binary(AST::BinaryExpr& bin);
    llvm::Value* emit_assign(AST::Expr& e, AST::AssignExpr& a);
    llvm::Value* emit_incdec(AST::IncDecExpr& x);
    llvm::Value* emit_call(AST::Expr& e, AST::CallExpr& c);
    llvm::Value* emit_builtin(AST::Expr& e, AST::CallExpr& c);
    llvm::Value* emit_cast(AST::Expr& e, AST::CastExpr& c);
    llvm::Value* emit_method(AST::Expr& e, AST::MethodCallExpr& m);
    llvm::Value* seq_elem_ptr(llvm::Value* hdr, llvm::Value* index, const Sema::Type* elem, int line);
    llvm::Value* seq_len(llvm::Value* hdr);
    llvm::Value* seq_data(llvm::Value* hdr);
    // A map key ready for the runtime: an i64 for integer keys, a pointer to a %vulp.vec for
    // string keys (the operand must be released after the call).
    Operand emit_map_key(AST::Expr& key, const Sema::Type* map_t, bool own = false);
    // Makes a borrowed operand an owned copy (needed when other code runs before it is used).
    void own_operand(Operand& op, const AST::Expr& e);
    llvm::Value* map_key_arg(const Operand& key, const Sema::Type* map_t);
    llvm::Value* map_insert_ptr(llvm::Value* slot, const Sema::Type* map_t, llvm::Value* key);
    llvm::Value* map_get_ptr(llvm::Value* map, const Sema::Type* map_t, llvm::Value* key);
    llvm::Value* widen_key(llvm::Value* key, const Sema::Type* key_t);
    void emit_bounds_check(llvm::Value* index, llvm::Value* len, int line);
    void emit_panic(const std::string& msg, int line);

    /*** CodeGenHelpers.cpp: conversions, deep copies, frees, printing, reading ***/
    llvm::Value* convert(llvm::Value* v, const Sema::Type* from, const Sema::Type* to);
    llvm::Value* to_i64(llvm::Value* v, const Sema::Type* t);
    llvm::Value* deep_copy(llvm::Value* v, const Sema::Type* t);
    llvm::Function* fix_fn(const Sema::Type* t);
    void emit_fix(llvm::Value* p, const Sema::Type* t);
    llvm::Function* free_fn(const Sema::Type* t);
    void emit_free(llvm::Value* p, const Sema::Type* t); // frees what the value at p owns
    void emit_free_elements(llvm::Value* hdr, const Sema::Type* seq_t);
    void emit_free_map_values(llvm::Value* map, const Sema::Type* map_t);
    llvm::Function* print_fn(const Sema::Type* t);
    void emit_print_value(llvm::Value* v, const Sema::Type* t);
    void emit_print_at(llvm::Value* p, const Sema::Type* t);
    void emit_print_bytes(const std::string& s);
    llvm::Function* read_fn(const Sema::Type* t);
    void emit_read_into(llvm::Value* p, const Sema::Type* t, llvm::Value* line);
    void emit_alloc_dims(llvm::Value* p, const Sema::Type* t, const std::vector<llvm::Value*>& sizes, size_t level,
                         int line);
    int32_t sort_kind(const Sema::Type* elem);
    // Returns the cached helper for `t`, or creates `void name(params)`, caches it (before the
    // body is generated, so recursive types work) and generates its body with `body(f)`.
    template<class F>
    llvm::Function* make_helper(std::unordered_map<const Sema::Type*, llvm::Function*>& cache, const Sema::Type* t,
                                const std::string& name, const std::vector<llvm::Type*>& params, F&& body);
    // Emits `for (i = 0; i < count; i++) body(i)` with an i64 counter.
    template<class F> void emit_counted_loop(llvm::Value* count, F&& body);

    /*** CodeGenStmt.cpp ***/
    void emit_stmt(AST::Stmt& s);
    void emit_var_decl(AST::VarDeclStmt& d, int line);
    void emit_if(AST::IfStmt& s);
    void emit_while(AST::WhileStmt& s);
    void emit_for_c(AST::ForCStmt& s);
    void emit_for_in(AST::ForInStmt& s);
    void emit_for_range(AST::ForInStmt& s, AST::RangeLit& range);
    void emit_tc(AST::TCStmt& s, int line);
    void emit_print(AST::PrintStmt& s);
    void emit_body(AST::Stmt& body, llvm::BasicBlock* next);
};

template<class F>
void CodeGen::emit_counted_loop(llvm::Value* count, F&& body) {
    llvm::Function* fn = b.GetInsertBlock()->getParent();
    llvm::AllocaInst* idx = entry_alloca(i64(), "i");
    b.CreateStore(c64(0), idx);
    llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(ctx, "loop.cond", fn);
    llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(ctx, "loop.body", fn);
    llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx, "loop.end", fn);
    b.CreateBr(cond_bb);
    b.SetInsertPoint(cond_bb);
    llvm::Value* i = b.CreateLoad(i64(), idx);
    b.CreateCondBr(b.CreateICmpSLT(i, count), body_bb, end_bb);
    b.SetInsertPoint(body_bb);
    body(i);
    b.CreateStore(b.CreateAdd(i, c64(1)), idx);
    b.CreateBr(cond_bb);
    b.SetInsertPoint(end_bb);
}

template<class F>
llvm::Function* CodeGen::make_helper(std::unordered_map<const Sema::Type*, llvm::Function*>& cache, const Sema::Type* t,
                                     const std::string& name, const std::vector<llvm::Type*>& params, F&& body) {
    if (const auto it = cache.find(t); it != cache.end()) return it->second;
    auto* ft = llvm::FunctionType::get(b.getVoidTy(), params, false);
    llvm::Function* f = llvm::Function::Create(ft, llvm::Function::InternalLinkage, name + t->mangle(), module.get());
    cache[t] = f;
    const auto saved_ip = b.saveIP();
    llvm::Function* saved_fn = cur_fn;
    cur_fn = f;
    b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", f));
    body(f);
    b.CreateRetVoid();
    cur_fn = saved_fn;
    b.restoreIP(saved_ip);
    return f;
}

#endif // VULP_CODEGEN_HPP
