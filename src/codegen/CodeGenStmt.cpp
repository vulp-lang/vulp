#include "codegen/CodeGen.hpp"

#include <optional>

using namespace AST;
using namespace Sema;

void CodeGen::emit_body(Stmt& body, llvm::BasicBlock* next) {
    emit_stmt(body);
    if (!block_terminated()) b.CreateBr(next);
}

void CodeGen::emit_stmt(Stmt& s) {
    const int line = s.loc.line;
    std::visit(overloaded{
        [&](ExprStmt& x) {
            llvm::Value* v = emit_expr(*x.expr);
            // an owned temporary nobody uses (e.g. `f();` or `v.pop();` returning a vec)
            const Type* t = x.expr->type;
            if (v && !t->is_trivial() && !is_borrowed(*x.expr)) emit_free(spill(v, "unused"), t);
        },
        [&](BlockStmt& blk) {
            for (auto& child : blk.stmts) {
                if (block_terminated()) break; // code after return/break/continue is dead
                emit_stmt(*child);
            }
        },
        [&](VarDeclStmt& d) { emit_var_decl(d, line); },
        [&](ReadStmt& r) {
            for (auto& target : r.targets) emit_read_into(emit_place(*target), target->type, c32(target->loc.line));
        },
        [&](PrintStmt& p) { emit_print(p); },
        [&](IfStmt& i) { emit_if(i); },
        [&](WhileStmt& w) { emit_while(w); },
        [&](ForInStmt& f) { emit_for_in(f); },
        [&](ForCStmt& f) { emit_for_c(f); },
        [&](TCStmt& t) { emit_tc(t, line); },
        [&](ReturnStmt& r) {
            if (r.value) b.CreateStore(emit_as(*r.value, cur_info->ret, true), ret_slot);
            b.CreateBr(exit_bb); // frees the function's owned locals, then returns
        },
        [&](BreakStmt&) { b.CreateBr(loops.back().break_bb); },
        [&](ContinueStmt&) { b.CreateBr(loops.back().continue_bb); },
        [&](FnDecl&) {},     // emitted separately
        [&](StructDecl&) {}, // types only
    }, s.node);
}

void CodeGen::emit_var_decl(VarDeclStmt& d, const int line) {
    const Type* t = d.resolved;
    std::vector<llvm::Value*> sizes; // evaluated once, outermost first
    for (auto& dim : d.type.dims) {
        if (dim) sizes.push_back(to_i64(emit_expr(*dim), dim->type));
    }
    for (auto& decl : d.decls) {
        const VarInfo* var = decl.var;
        llvm::Value* slot;
        if (var->is_global) {
            slot = var_storage.at(var);
        } else {
            slot = zeroed_alloca(t, decl.name);
            var_storage[var] = slot;
            if (!t->is_trivial()) owned_locals.emplace_back(slot, t);
        }
        // A declaration that runs again (inside a loop or TC block) frees the previous value.
        if (decl.init) {
            llvm::Value* v = emit_as(*decl.init, t, true);
            emit_free(slot, t);
            b.CreateStore(v, slot);
            continue;
        }
        emit_free(slot, t);
        b.CreateStore(llvm::Constant::getNullValue(lty(t)), slot); // every declaration starts from zero
        if (!sizes.empty()) emit_alloc_dims(slot, t, sizes, 0, line);
        if (d.from_read) emit_read_into(slot, t, c32(line));
    }
}

void CodeGen::emit_print(PrintStmt& p) {
    for (auto& arg : p.args) {
        if (const auto* lit = std::get_if<StringLit>(&arg->node)) {
            emit_print_bytes(lit->value); // no allocation for literals
            continue;
        }
        const Operand op = emit_operand(*arg);
        emit_print_value(op.value, arg->type);
        release_operand(op);
    }
    if (p.newline) b.CreateCall(rt("vulp_print_char"), {c32('\n')});
}

void CodeGen::emit_if(IfStmt& s) {
    llvm::Value* cond = emit_expr(*s.cond);
    llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(ctx, "if.then", cur_fn);
    llvm::BasicBlock* else_bb = s.else_branch ? llvm::BasicBlock::Create(ctx, "if.else", cur_fn) : nullptr;
    llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx, "if.end", cur_fn);
    b.CreateCondBr(cond, then_bb, else_bb ? else_bb : end_bb);
    b.SetInsertPoint(then_bb);
    emit_body(*s.then_branch, end_bb);
    if (else_bb) {
        b.SetInsertPoint(else_bb);
        emit_body(*s.else_branch, end_bb);
    }
    b.SetInsertPoint(end_bb);
}

void CodeGen::emit_while(WhileStmt& s) {
    llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(ctx, "while.cond", cur_fn);
    llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(ctx, "while.body", cur_fn);
    llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx, "while.end", cur_fn);
    b.CreateBr(cond_bb);
    b.SetInsertPoint(cond_bb);
    b.CreateCondBr(emit_expr(*s.cond), body_bb, end_bb);
    b.SetInsertPoint(body_bb);
    loops.push_back({end_bb, cond_bb});
    emit_body(*s.body, cond_bb);
    loops.pop_back();
    b.SetInsertPoint(end_bb);
}

void CodeGen::emit_for_c(ForCStmt& s) {
    if (s.init) emit_stmt(*s.init);
    llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(ctx, "for.cond", cur_fn);
    llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(ctx, "for.body", cur_fn);
    llvm::BasicBlock* step_bb = llvm::BasicBlock::Create(ctx, "for.step", cur_fn);
    llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx, "for.end", cur_fn);
    b.CreateBr(cond_bb);
    b.SetInsertPoint(cond_bb);
    if (s.cond) b.CreateCondBr(emit_expr(*s.cond), body_bb, end_bb);
    else b.CreateBr(body_bb);
    b.SetInsertPoint(body_bb);
    loops.push_back({end_bb, step_bb});
    emit_body(*s.body, step_bb);
    loops.pop_back();
    b.SetInsertPoint(step_bb);
    if (s.step) {
        llvm::Value* v = emit_expr(*s.step);
        const Type* t = s.step->type;
        if (v && !t->is_trivial() && !is_borrowed(*s.step)) emit_free(spill(v, "unused"), t);
    }
    b.CreateBr(cond_bb);
    b.SetInsertPoint(end_bb);
}

// for x in [lo..hi]: counts from lo to hi inclusive without materialising an array. The
// bounds are evaluated once; assigning to x inside the body does not affect the iteration.
void CodeGen::emit_for_range(ForInStmt& s, RangeLit& range) {
    const Type* t = s.var->type;
    llvm::Value* lo = emit_as(*range.lo, t, false);
    llvm::Value* hi = emit_as(*range.hi, t, false);
    const bool sign = t->kind == TypeKind::Int && t->is_signed;
    llvm::AllocaInst* var = entry_alloca(lty(t), s.var_name);
    var_storage[s.var] = var;
    llvm::AllocaInst* counter = entry_alloca(lty(t), "range.i");
    b.CreateStore(lo, counter);

    llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(ctx, "range.body", cur_fn);
    llvm::BasicBlock* step_bb = llvm::BasicBlock::Create(ctx, "range.step", cur_fn);
    llvm::BasicBlock* inc_bb = llvm::BasicBlock::Create(ctx, "range.inc", cur_fn);
    llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx, "range.end", cur_fn);
    b.CreateCondBr(sign ? b.CreateICmpSLE(lo, hi) : b.CreateICmpULE(lo, hi), body_bb, end_bb);

    b.SetInsertPoint(body_bb);
    b.CreateStore(b.CreateLoad(lty(t), counter), var);
    loops.push_back({end_bb, step_bb});
    emit_body(*s.body, step_bb);
    loops.pop_back();

    b.SetInsertPoint(step_bb); // stop after hi (works even when hi is the type's maximum)
    llvm::Value* cur = b.CreateLoad(lty(t), counter);
    b.CreateCondBr(b.CreateICmpEQ(cur, hi), end_bb, inc_bb);
    b.SetInsertPoint(inc_bb);
    b.CreateStore(b.CreateAdd(cur, llvm::ConstantInt::get(lty(t), 1)), counter);
    b.CreateBr(body_bb);
    b.SetInsertPoint(end_bb);
}

// for x in seq / map / string. The sequence walked is
//  - a place (`v`, `adj[u]`, `g.rows[i]`): its index values are evaluated once, and its
//    address is recomputed from them every iteration, so the loop sees the live length and
//    never keeps a pointer into a buffer the body could reallocate or free;
//  - otherwise an owned snapshot (a temporary, or a map's sorted keys) that is freed when the
//    loop ends.
void CodeGen::emit_for_in(ForInStmt& s) {
    if (auto* range = std::get_if<RangeLit>(&s.iterable->node)) {
        emit_for_range(s, *range);
        return;
    }
    const Type* it = s.iterable->type;
    const Type* var_t = s.var->type;

    llvm::AllocaInst* snapshot = nullptr;
    const Type* snapshot_t = nullptr;
    std::optional<FrozenPlace> place;
    const Type* elem_t;
    if (it->kind == TypeKind::Map) {
        elem_t = var_t->kind == TypeKind::String ? types.string_t : types.int_type(64, true);
        snapshot_t = types.vec_of(elem_t);
        snapshot = zeroed_alloca(snapshot_t, "keys");
        llvm::Value* map = is_place(*s.iterable) ? b.CreateLoad(ptr(), emit_place(*s.iterable, false))
                                                 : emit_expr(*s.iterable);
        emit_free(snapshot, snapshot_t);
        b.CreateCall(rt("vulp_map_keys"), {map, snapshot});
    } else {
        elem_t = it->element();
        if (is_place(*s.iterable)) {
            place = freeze_place(*s.iterable);
        } else {
            snapshot_t = it;
            snapshot = zeroed_alloca(snapshot_t, "iter");
            llvm::Value* v = emit_owned(*s.iterable);
            emit_free(snapshot, snapshot_t);
            b.CreateStore(v, snapshot);
        }
    }
    const auto sequence = [&]() -> llvm::Value* {
        return snapshot ? snapshot : frozen_address(*place);
    };

    llvm::AllocaInst* var = zeroed_alloca(var_t, s.var_name);
    var_storage[s.var] = var;
    if (!var_t->is_trivial()) owned_locals.emplace_back(var, var_t);
    llvm::AllocaInst* idx = entry_alloca(i64(), "idx");
    b.CreateStore(c64(0), idx);

    llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(ctx, "forin.cond", cur_fn);
    llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(ctx, "forin.body", cur_fn);
    llvm::BasicBlock* step_bb = llvm::BasicBlock::Create(ctx, "forin.step", cur_fn);
    llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx, "forin.end", cur_fn);
    b.CreateBr(cond_bb);
    b.SetInsertPoint(cond_bb);
    llvm::Value* i = b.CreateLoad(i64(), idx);
    b.CreateCondBr(b.CreateICmpSLT(i, seq_len(sequence())), body_bb, end_bb);

    b.SetInsertPoint(body_bb);
    llvm::Value* elem = b.CreateLoad(lty(elem_t), b.CreateGEP(lty(elem_t), seq_data(sequence()), i));
    if (it->kind == TypeKind::Map && var_t->kind != TypeKind::String) {
        elem = b.CreateTrunc(elem, lty(var_t)); // integer keys come back as i64
    } else {
        elem = deep_copy(elem, elem_t);         // the loop variable is a copy of the element
    }
    emit_free(var, var_t);
    b.CreateStore(elem, var);
    loops.push_back({end_bb, step_bb});
    emit_body(*s.body, step_bb);
    loops.pop_back();

    b.SetInsertPoint(step_bb);
    b.CreateStore(b.CreateAdd(b.CreateLoad(i64(), idx), c64(1)), idx);
    b.CreateBr(cond_bb);
    b.SetInsertPoint(end_bb);
    if (place) release_frozen(*place);
    if (snapshot) {
        emit_free(snapshot, snapshot_t);
        b.CreateStore(llvm::Constant::getNullValue(lty(snapshot_t)), snapshot);
    }
}

// TC: read the number of test cases, then run the body that many times. Caches of dp
// functions that read globals are cleared before each test case.
void CodeGen::emit_tc(TCStmt& s, const int line) {
    llvm::Value* count = b.CreateCall(rt("vulp_read_i64"), {c32(line)}, "tc.count");
    llvm::AllocaInst* idx = entry_alloca(i64(), "tc.i");
    b.CreateStore(c64(0), idx);
    llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(ctx, "tc.cond", cur_fn);
    llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(ctx, "tc.body", cur_fn);
    llvm::BasicBlock* step_bb = llvm::BasicBlock::Create(ctx, "tc.step", cur_fn);
    llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx, "tc.end", cur_fn);
    b.CreateBr(cond_bb);
    b.SetInsertPoint(cond_bb);
    b.CreateCondBr(b.CreateICmpSLT(b.CreateLoad(i64(), idx), count), body_bb, end_bb);

    b.SetInsertPoint(body_bb);
    for (const auto& fn : info.fns) {
        if (fn->is_dp && fn->reset_cache_per_testcase) {
            b.CreateCall(rt("vulp_memo_clear"), {b.CreateLoad(ptr(), dp_memo.at(fn.get()))});
        }
    }
    loops.push_back({end_bb, step_bb});
    emit_body(*s.body, step_bb);
    loops.pop_back();

    b.SetInsertPoint(step_bb);
    b.CreateStore(b.CreateAdd(b.CreateLoad(i64(), idx), c64(1)), idx);
    b.CreateBr(cond_bb);
    b.SetInsertPoint(end_bb);
}
