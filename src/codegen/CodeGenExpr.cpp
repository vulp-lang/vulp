#include "codegen/CodeGen.hpp"

#include <cmath>
#include <cstdlib>
#include <optional>

#include <llvm/IR/Intrinsics.h>

using namespace AST;
using namespace Sema;

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

    bool is_signed_int(const Type* t) {
        return t->kind == TypeKind::Int && t->is_signed;
    }

    bool is_comparison(const TokenType op) {
        switch (op) {
            case TokenType::EQUAL_EQUAL:
            case TokenType::NOT_EQUAL:
            case TokenType::LESS:
            case TokenType::LESS_EQUAL:
            case TokenType::GREATER:
            case TokenType::GREATER_EQUAL:
                return true;
            default:
                return false;
        }
    }
} // namespace

/*** places and ownership ***/

bool CodeGen::is_place(const Expr& e) {
    if (std::holds_alternative<NameExpr>(e.node)) return true;
    if (const auto* ix = std::get_if<IndexExpr>(&e.node)) return is_place(*ix->base);
    if (const auto* f = std::get_if<FieldExpr>(&e.node)) return is_place(*f->base);
    return false;
}

const Expr& CodeGen::chain_root(const Expr& e) {
    if (const auto* ix = std::get_if<IndexExpr>(&e.node)) return chain_root(*ix->base);
    if (const auto* f = std::get_if<FieldExpr>(&e.node)) return chain_root(*f->base);
    return e;
}

// Expressions whose value still shares heap buffers with some variable, element or field.
// Everything else (calls, literals, concatenations, elements copied out of temporaries) is an
// owned temporary.
bool CodeGen::is_borrowed(const Expr& e) {
    if (std::holds_alternative<NameExpr>(e.node) || std::holds_alternative<AssignExpr>(e.node)) return true;
    if (std::holds_alternative<IndexExpr>(e.node) || std::holds_alternative<FieldExpr>(e.node)) {
        return is_borrowed(chain_root(e));
    }
    if (const auto* m = std::get_if<MethodCallExpr>(&e.node)) {
        return m->resolved == Method::Back && is_borrowed(chain_root(*m->base));
    }
    return false;
}

template<class F>
llvm::Value* CodeGen::with_temp_place(Expr& e, F&& use) {
    std::vector<Expr*> chain; // from e down to (excluding) the root
    Expr* cur = &e;
    while (true) {
        if (auto* ix = std::get_if<IndexExpr>(&cur->node)) {
            chain.push_back(cur);
            cur = ix->base.get();
        } else if (auto* f = std::get_if<FieldExpr>(&cur->node)) {
            chain.push_back(cur);
            cur = f->base.get();
        } else {
            break;
        }
    }
    const Operand root = emit_operand(*cur);
    llvm::Value* p = root.slot ? root.slot : spill(root.value, "temp");
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        Expr& step = **it;
        if (auto* f = std::get_if<FieldExpr>(&step.node)) {
            p = b.CreateStructGEP(struct_types.at(f->base->type->st), p, static_cast<unsigned>(f->field_index));
            continue;
        }
        auto& ix = std::get<IndexExpr>(step.node);
        const Type* bt = ix.base->type;
        if (bt->kind == TypeKind::Map) {
            const Operand key = emit_map_key(*ix.index, bt);
            llvm::Value* found = map_get_ptr(b.CreateLoad(ptr(), p), bt, map_key_arg(key, bt));
            llvm::AllocaInst* zero = entry_alloca(lty(bt->val), "missing");
            b.CreateStore(llvm::Constant::getNullValue(lty(bt->val)), zero);
            p = b.CreateSelect(b.CreateIsNull(found), zero, found);
            release_operand(key);
        } else {
            llvm::Value* idx = to_i64(emit_expr(*ix.index), ix.index->type);
            p = seq_elem_ptr(p, idx, bt->element(), step.loc.line);
        }
    }
    llvm::Value* result = use(p, root.slot != nullptr);
    release_operand(root);
    return result;
}

// Conservative: can evaluating `e` modify any variable (and so free or move a buffer)?
bool CodeGen::has_side_effects(const Expr& e) {
    return std::visit(overloaded{
        [](const ArrayLit& x) {
            for (const auto& el : x.elems) {
                if (has_side_effects(*el)) return true;
            }
            return false;
        },
        [](const RangeLit& x) { return has_side_effects(*x.lo) || has_side_effects(*x.hi); },
        [](const UnaryExpr& x) { return has_side_effects(*x.operand); },
        [](const BinaryExpr& x) { return has_side_effects(*x.lhs) || has_side_effects(*x.rhs); },
        [](const AssignExpr&) { return true; },
        [](const IncDecExpr&) { return true; },
        [](const CallExpr& x) {
            if (x.kind == CallKind::Function || x.builtin == Builtin::Exit) return true;
            for (const auto& a : x.args) {
                if (has_side_effects(*a)) return true;
            }
            return false;
        },
        [](const CastExpr& x) { return has_side_effects(*x.operand); },
        [](const IndexExpr& x) { return has_side_effects(*x.base) || has_side_effects(*x.index); },
        [](const FieldExpr& x) { return has_side_effects(*x.base); },
        [](const MethodCallExpr& x) {
            if (x.resolved != Method::Len && x.resolved != Method::Back && x.resolved != Method::Has) return true;
            if (has_side_effects(*x.base)) return true;
            for (const auto& a : x.args) {
                if (has_side_effects(*a)) return true;
            }
            return false;
        },
        [](const auto&) { return false; }, // literals and names
    }, e.node);
}

CodeGen::FrozenPlace CodeGen::freeze_place(Expr& e) {
    if (auto* n = std::get_if<NameExpr>(&e.node)) {
        FrozenPlace place;
        place.root = var_storage.at(n->var);
        return place;
    }
    if (auto* f = std::get_if<FieldExpr>(&e.node)) {
        FrozenPlace place = freeze_place(*f->base);
        FrozenPlace::Step step{};
        step.kind = FrozenPlace::Step::Kind::Field;
        step.base_type = f->base->type;
        step.field = static_cast<unsigned>(f->field_index);
        place.steps.push_back(step);
        return place;
    }
    auto& ix = std::get<IndexExpr>(e.node);
    FrozenPlace place = freeze_place(*ix.base);
    const Type* bt = ix.base->type;
    FrozenPlace::Step step{};
    step.kind = FrozenPlace::Step::Kind::Index;
    step.base_type = bt;
    step.line = e.loc.line;
    if (bt->kind == TypeKind::Map) {
        step.kind = FrozenPlace::Step::Kind::Key;
        if (bt->key->kind == TypeKind::String && is_borrowed(*ix.index)) {
            // keep a private copy: the variable the key came from may change meanwhile
            Operand key;
            key.type = bt->key;
            key.slot = spill(deep_copy(emit_expr(*ix.index), bt->key), "key");
            key.value = key.slot;
            step.key = key;
        } else {
            step.key = emit_map_key(*ix.index, bt);
        }
    } else {
        step.index = to_i64(emit_expr(*ix.index), ix.index->type);
    }
    place.steps.push_back(step);
    return place;
}

llvm::Value* CodeGen::frozen_address(const FrozenPlace& place, const bool write) {
    llvm::Value* p = place.root;
    for (const auto& step : place.steps) {
        switch (step.kind) {
            case FrozenPlace::Step::Kind::Field:
                p = b.CreateStructGEP(struct_types.at(step.base_type->st), p, step.field);
                break;
            case FrozenPlace::Step::Kind::Index:
                p = seq_elem_ptr(p, step.index, step.base_type->element(), step.line);
                break;
            case FrozenPlace::Step::Kind::Key: {
                if (write) {
                    p = map_insert_ptr(p, step.base_type, step.key.value);
                    break;
                }
                const Type* val = step.base_type->val;
                llvm::Value* found = map_get_ptr(b.CreateLoad(ptr(), p), step.base_type, step.key.value);
                llvm::AllocaInst* zero = entry_alloca(lty(val), "missing");
                b.CreateStore(llvm::Constant::getNullValue(lty(val)), zero);
                p = b.CreateSelect(b.CreateIsNull(found), zero, found);
                break;
            }
        }
    }
    return p;
}

void CodeGen::release_frozen(const FrozenPlace& place) {
    for (const auto& step : place.steps) {
        if (step.kind == FrozenPlace::Step::Kind::Key) release_operand(step.key);
    }
}

llvm::Value* CodeGen::emit_owned(Expr& e) {
    llvm::Value* v = emit_expr(e);
    if (!e.type->is_trivial() && is_borrowed(e)) v = deep_copy(v, e.type);
    return v;
}

llvm::Value* CodeGen::emit_as(Expr& e, const Type* to, const bool owned) {
    llvm::Value* v = owned ? emit_owned(e) : emit_expr(e);
    return convert(v, e.type, to);
}

CodeGen::Operand CodeGen::emit_operand(Expr& e) {
    Operand op;
    op.type = e.type;
    if (const auto* lit = std::get_if<StringLit>(&e.node)) {
        op.value = b.CreateLoad(vec_ty, string_header(lit->value)); // no allocation for literals
        return op;
    }
    op.value = emit_expr(e);
    if (!e.type->is_trivial() && !is_borrowed(e)) op.slot = spill(op.value, "operand");
    return op;
}

void CodeGen::release_operand(const Operand& op) {
    if (op.slot) emit_free(op.slot, op.type);
}

void CodeGen::own_operand(Operand& op, const Expr& e) {
    if (op.slot || op.type->is_trivial() || !is_borrowed(e)) return; // already owned, or a literal
    op.value = deep_copy(op.value, op.type);
    op.slot = spill(op.value, "owned");
}

// The address of a place. All index / key expressions are evaluated first (outermost
// container first); the address is computed afterwards with no user code in between, so an
// index expression that resizes a container (`a[0][f()]`) cannot leave the address dangling.
llvm::Value* CodeGen::emit_place(Expr& e, const bool write) {
    if (auto* n = std::get_if<NameExpr>(&e.node)) return var_storage.at(n->var);
    if (is_place(e)) {
        const FrozenPlace place = freeze_place(e);
        llvm::Value* p = frozen_address(place, write);
        release_frozen(place);
        return p;
    }
    return spill(emit_expr(e)); // temporaries live in a stack slot
}

/*** sequences & maps ***/

llvm::Value* CodeGen::seq_len(llvm::Value* hdr) {
    return b.CreateLoad(i64(), b.CreateStructGEP(vec_ty, hdr, 1), "len");
}

llvm::Value* CodeGen::seq_data(llvm::Value* hdr) {
    return b.CreateLoad(ptr(), b.CreateStructGEP(vec_ty, hdr, 0), "data");
}

void CodeGen::emit_bounds_check(llvm::Value* index, llvm::Value* len, const int line) {
    if (!opts.runtime_checks) return;
    llvm::Function* fn = b.GetInsertBlock()->getParent();
    llvm::BasicBlock* fail = llvm::BasicBlock::Create(ctx, "oob", fn);
    llvm::BasicBlock* ok = llvm::BasicBlock::Create(ctx, "inbounds", fn);
    b.CreateCondBr(b.CreateICmpULT(index, len), ok, fail);
    b.SetInsertPoint(fail);
    b.CreateCall(rt("vulp_rt_bounds_fail"), {index, len, c32(line)});
    b.CreateUnreachable();
    b.SetInsertPoint(ok);
}

llvm::Value* CodeGen::seq_elem_ptr(llvm::Value* hdr, llvm::Value* index, const Type* elem, const int line) {
    llvm::Value* len = seq_len(hdr);
    emit_bounds_check(index, len, line);
    return b.CreateGEP(lty(elem), seq_data(hdr), index, "elem");
}

llvm::Value* CodeGen::widen_key(llvm::Value* key, const Type* key_t) {
    if (is_signed_int(key_t)) return b.CreateSExtOrTrunc(key, i64());
    return b.CreateZExtOrTrunc(key, i64());
}

// `own`: the key is used only after other code runs (e.g. the receiver's index expressions),
// which could overwrite a borrowed key, so take a copy.
CodeGen::Operand CodeGen::emit_map_key(Expr& key, const Type* map_t, const bool own) {
    if (map_t->key->kind == TypeKind::String) {
        Operand op = emit_operand(key);
        if (own) own_operand(op, key);
        op.value = op.slot ? op.slot : spill(op.value, "key"); // runtime takes a pointer
        return op;
    }
    Operand op;
    op.type = map_t->key;
    op.value = widen_key(emit_as(key, map_t->key, false), map_t->key);
    return op;
}

llvm::Value* CodeGen::map_key_arg(const Operand& key, const Type*) {
    return key.value;
}

llvm::Value* CodeGen::map_insert_ptr(llvm::Value* slot, const Type* map_t, llvm::Value* key) {
    const auto val_size = static_cast<int64_t>(size_of(map_t->val));
    if (map_t->key->kind == TypeKind::String) return b.CreateCall(rt("vulp_map_index_s"), {slot, c64(val_size), key});
    const int kind = is_signed_int(map_t->key) ? 0 : 1;
    return b.CreateCall(rt("vulp_map_index_i"), {slot, c32(kind), c64(val_size), key});
}

llvm::Value* CodeGen::map_get_ptr(llvm::Value* map, const Type* map_t, llvm::Value* key) {
    return b.CreateCall(rt(map_t->key->kind == TypeKind::String ? "vulp_map_get_s" : "vulp_map_get_i"), {map, key});
}

/*** expressions ***/

// The value of `f()[i]`, `mk().field`, ...: copied out of the temporary, which is then freed.
llvm::Value* CodeGen::element_of_temporary(Expr& e) {
    return with_temp_place(e, [&](llvm::Value* p, const bool owned) -> llvm::Value* {
        llvm::Value* v = b.CreateLoad(lty(e.type), p);
        return owned ? deep_copy(v, e.type) : v;
    });
}

llvm::Value* CodeGen::emit_expr(Expr& e) {
    return std::visit(overloaded{
        [&](IntLit& lit) -> llvm::Value* { return emit_int_lit(e, lit); },
        [&](FloatLit& lit) -> llvm::Value* {
            return llvm::ConstantFP::get(b.getDoubleTy(), std::strtod(lit.text.c_str(), nullptr));
        },
        [&](CharLit& lit) -> llvm::Value* { return b.getInt8(lit.value); },
        [&](StringLit& lit) -> llvm::Value* { return emit_string_lit(lit.value); },
        [&](BoolLit& lit) -> llvm::Value* { return b.getInt1(lit.value); },
        [&](ArrayLit& lit) -> llvm::Value* { return emit_array_lit(e, lit); },
        [&](RangeLit& lit) -> llvm::Value* { return emit_range_lit(e, lit); },
        [&](NameExpr& n) -> llvm::Value* { return b.CreateLoad(lty(e.type), var_storage.at(n.var), n.name); },
        [&](UnaryExpr& u) -> llvm::Value* { return emit_unary(e, u); },
        [&](BinaryExpr& bin) -> llvm::Value* { return emit_binary(e, bin); },
        [&](AssignExpr& a) -> llvm::Value* { return emit_assign(e, a); },
        [&](IncDecExpr& x) -> llvm::Value* { return emit_incdec(x); },
        [&](CallExpr& c) -> llvm::Value* { return emit_call(e, c); },
        [&](CastExpr& c) -> llvm::Value* { return emit_cast(e, c); },
        [&](IndexExpr&) -> llvm::Value* {
            if (!is_place(e)) return element_of_temporary(e);
            return b.CreateLoad(lty(e.type), emit_place(e, false));
        },
        [&](FieldExpr&) -> llvm::Value* {
            if (!is_place(e)) return element_of_temporary(e);
            return b.CreateLoad(lty(e.type), emit_place(e, false));
        },
        [&](MethodCallExpr& m) -> llvm::Value* { return emit_method(e, m); },
    }, e.node);
}

llvm::Value* CodeGen::emit_int_lit(Expr& e, const IntLit& lit) {
    const unsigned bits = lty(e.type)->getIntegerBitWidth();
    return llvm::ConstantInt::get(ctx, lit.value.trunc(bits));
}

llvm::Value* CodeGen::emit_string_lit(const std::string& s) {
    llvm::AllocaInst* tmp = entry_alloca(vec_ty, "strlit");
    b.CreateCall(rt("vulp_str_from_bytes"), {tmp, string_constant(s), c64(static_cast<int64_t>(s.size()))});
    return b.CreateLoad(vec_ty, tmp);
}

llvm::Value* CodeGen::emit_array_lit(Expr& e, ArrayLit& lit) {
    const Type* elem = e.type->elem;
    llvm::AllocaInst* tmp = entry_alloca(vec_ty, "arrlit");
    b.CreateCall(rt("vulp_vec_alloc"), {tmp, c64(static_cast<int64_t>(lit.elems.size())),
                                        c64(static_cast<int64_t>(size_of(elem))), c32(e.loc.line)});
    for (size_t i = 0; i < lit.elems.size(); i++) {
        llvm::Value* v = emit_as(*lit.elems[i], elem, true);
        llvm::Value* slot = b.CreateGEP(lty(elem), seq_data(tmp), c64(static_cast<int64_t>(i)));
        b.CreateStore(v, slot);
    }
    return b.CreateLoad(vec_ty, tmp);
}

// [lo..hi] materialised as an array (inclusive; empty when lo > hi).
llvm::Value* CodeGen::emit_range_lit(Expr& e, RangeLit& lit) {
    const Type* elem = e.type->elem;
    llvm::Value* lo = emit_as(*lit.lo, elem, false);
    llvm::Value* hi = emit_as(*lit.hi, elem, false);
    const bool sign = is_signed_int(elem);
    llvm::Value* empty = sign ? b.CreateICmpSGT(lo, hi) : b.CreateICmpUGT(lo, hi);
    llvm::Value* span = b.CreateZExtOrTrunc(b.CreateSub(hi, lo), i64());
    llvm::Value* count = b.CreateSelect(empty, c64(0), b.CreateAdd(span, c64(1)), "count");
    llvm::AllocaInst* tmp = entry_alloca(vec_ty, "range");
    b.CreateCall(rt("vulp_vec_alloc"), {tmp, count, c64(static_cast<int64_t>(size_of(elem))), c32(e.loc.line)});
    llvm::Value* data = seq_data(tmp);
    emit_counted_loop(count, [&](llvm::Value* i) {
        llvm::Value* v = b.CreateAdd(lo, b.CreateZExtOrTrunc(i, lty(elem)));
        b.CreateStore(v, b.CreateGEP(lty(elem), data, i));
    });
    return b.CreateLoad(vec_ty, tmp);
}

llvm::Value* CodeGen::emit_unary(Expr& e, UnaryExpr& u) {
    llvm::Value* v = emit_as(*u.operand, e.type, false);
    switch (u.op) {
        case TokenType::MINUS:
            return e.type->kind == TypeKind::Float ? b.CreateFNeg(v) : b.CreateNeg(v);
        case TokenType::NOT:
        case TokenType::TILDE:
            return b.CreateNot(v);
        default:
            throw CodeGenError("unknown unary operator");
    }
}

llvm::Value* CodeGen::emit_logical(BinaryExpr& bin) {
    const bool is_and = bin.op == TokenType::AND_AND;
    llvm::Value* l = emit_expr(*bin.lhs);
    llvm::BasicBlock* lhs_end = b.GetInsertBlock();
    llvm::Function* fn = lhs_end->getParent();
    llvm::BasicBlock* rhs_bb = llvm::BasicBlock::Create(ctx, is_and ? "and.rhs" : "or.rhs", fn);
    llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx, is_and ? "and.end" : "or.end", fn);
    if (is_and) b.CreateCondBr(l, rhs_bb, end_bb);
    else b.CreateCondBr(l, end_bb, rhs_bb);
    b.SetInsertPoint(rhs_bb);
    llvm::Value* r = emit_expr(*bin.rhs);
    llvm::BasicBlock* rhs_end = b.GetInsertBlock();
    b.CreateBr(end_bb);
    b.SetInsertPoint(end_bb);
    llvm::PHINode* phi = b.CreatePHI(b.getInt1Ty(), 2);
    phi->addIncoming(b.getInt1(!is_and), lhs_end);
    phi->addIncoming(r, rhs_end);
    return phi;
}

llvm::Value* CodeGen::emit_compare(const TokenType op, llvm::Value* l, llvm::Value* r, const Type* t) {
    if (t->kind == TypeKind::Float) {
        switch (op) {
            case TokenType::EQUAL_EQUAL: return b.CreateFCmpOEQ(l, r);
            case TokenType::NOT_EQUAL: return b.CreateFCmpUNE(l, r);
            case TokenType::LESS: return b.CreateFCmpOLT(l, r);
            case TokenType::LESS_EQUAL: return b.CreateFCmpOLE(l, r);
            case TokenType::GREATER: return b.CreateFCmpOGT(l, r);
            case TokenType::GREATER_EQUAL: return b.CreateFCmpOGE(l, r);
            default: break;
        }
    } else {
        const bool sign = is_signed_int(t);
        switch (op) {
            case TokenType::EQUAL_EQUAL: return b.CreateICmpEQ(l, r);
            case TokenType::NOT_EQUAL: return b.CreateICmpNE(l, r);
            case TokenType::LESS: return sign ? b.CreateICmpSLT(l, r) : b.CreateICmpULT(l, r);
            case TokenType::LESS_EQUAL: return sign ? b.CreateICmpSLE(l, r) : b.CreateICmpULE(l, r);
            case TokenType::GREATER: return sign ? b.CreateICmpSGT(l, r) : b.CreateICmpUGT(l, r);
            case TokenType::GREATER_EQUAL: return sign ? b.CreateICmpSGE(l, r) : b.CreateICmpUGE(l, r);
            default: break;
        }
    }
    throw CodeGenError("unknown comparison operator");
}

llvm::Value* CodeGen::emit_binop(const TokenType op, llvm::Value* l, llvm::Value* r, const Type* t, const int line) {
    if (t->kind == TypeKind::Float) {
        switch (op) {
            case TokenType::PLUS: return b.CreateFAdd(l, r);
            case TokenType::MINUS: return b.CreateFSub(l, r);
            case TokenType::STAR: return b.CreateFMul(l, r);
            case TokenType::SLASH: return b.CreateFDiv(l, r);
            default: throw CodeGenError("invalid float operator");
        }
    }
    const bool sign = is_signed_int(t);
    llvm::Type* ty = l->getType();
    const unsigned bits = ty->getIntegerBitWidth();
    switch (op) {
        case TokenType::PLUS: return b.CreateAdd(l, r);   // no nsw/nuw: overflow wraps
        case TokenType::MINUS: return b.CreateSub(l, r);
        case TokenType::STAR: return b.CreateMul(l, r);
        case TokenType::SLASH:
        case TokenType::PERCENT: {
            if (opts.runtime_checks) {
                llvm::Function* fn = b.GetInsertBlock()->getParent();
                llvm::BasicBlock* fail = llvm::BasicBlock::Create(ctx, "divzero", fn);
                llvm::BasicBlock* ok = llvm::BasicBlock::Create(ctx, "divok", fn);
                b.CreateCondBr(b.CreateICmpEQ(r, llvm::ConstantInt::get(ty, 0)), fail, ok);
                b.SetInsertPoint(fail);
                b.CreateCall(rt("vulp_rt_div_zero"), {c32(line)});
                b.CreateUnreachable();
                b.SetInsertPoint(ok);
            }
            if (sign) {
                // MIN / -1 overflows; Vulp defines it to wrap (quotient MIN, remainder 0), which
                // is exactly what dividing by 1 gives.
                llvm::Value* min = llvm::ConstantInt::get(ctx, llvm::APInt::getSignedMinValue(bits));
                llvm::Value* overflow = b.CreateAnd(b.CreateICmpEQ(l, min),
                                                    b.CreateICmpEQ(r, llvm::ConstantInt::getSigned(ty, -1)));
                r = b.CreateSelect(overflow, llvm::ConstantInt::get(ty, 1), r);
                return op == TokenType::SLASH ? b.CreateSDiv(l, r) : b.CreateSRem(l, r);
            }
            return op == TokenType::SLASH ? b.CreateUDiv(l, r) : b.CreateURem(l, r);
        }
        case TokenType::AMP: return b.CreateAnd(l, r);
        case TokenType::PIPE: return b.CreateOr(l, r);
        case TokenType::CARET: return b.CreateXor(l, r);
        case TokenType::SHL:
        case TokenType::SHR: {
            // Shifting by the bit width or more (or by a negative amount) shifts everything
            // out: the result is 0, or -1 for `>>` of a negative signed value.
            llvm::Value* too_far = b.CreateICmpUGE(r, llvm::ConstantInt::get(ty, bits));
            llvm::Value* amount = b.CreateSelect(too_far, llvm::ConstantInt::get(ty, 0), r);
            if (op == TokenType::SHL) {
                return b.CreateSelect(too_far, llvm::ConstantInt::get(ty, 0), b.CreateShl(l, amount));
            }
            if (sign) {
                return b.CreateSelect(too_far, b.CreateAShr(l, llvm::ConstantInt::get(ty, bits - 1)),
                                      b.CreateAShr(l, amount));
            }
            return b.CreateSelect(too_far, llvm::ConstantInt::get(ty, 0), b.CreateLShr(l, amount));
        }
        default: throw CodeGenError("invalid integer operator");
    }
}

// String comparison and concatenation. Temporaries are freed after use; literals are read
// in place without allocating.
llvm::Value* CodeGen::emit_string_binary(BinaryExpr& bin) {
    Operand l = emit_operand(*bin.lhs);
    if (!l.slot && is_borrowed(*bin.lhs) && has_side_effects(*bin.rhs)) {
        // the right side could overwrite (and free) the string the left side borrows
        l.value = deep_copy(l.value, types.string_t);
        l.slot = spill(l.value, "lhs");
    }
    llvm::Value* lp = l.slot ? l.slot : spill(l.value, "lhs");
    llvm::Value* result;
    if (bin.op == TokenType::PLUS) {
        llvm::AllocaInst* dst = entry_alloca(vec_ty, "concat");
        if (bin.rhs->type->kind == TypeKind::Char) {
            llvm::Value* c = emit_expr(*bin.rhs);
            b.CreateCall(rt("vulp_vec_clone"), {dst, lp, c64(1)});
            b.CreateStore(c, b.CreateCall(rt("vulp_vec_push_slot"), {dst, c64(1)}));
        } else {
            const Operand r = emit_operand(*bin.rhs);
            b.CreateCall(rt("vulp_str_concat"), {dst, lp, r.slot ? r.slot : spill(r.value, "rhs")});
            release_operand(r);
        }
        result = b.CreateLoad(vec_ty, dst);
    } else {
        const Operand r = emit_operand(*bin.rhs);
        llvm::Value* cmp = b.CreateCall(rt("vulp_str_cmp"), {lp, r.slot ? r.slot : spill(r.value, "rhs")});
        release_operand(r);
        result = emit_compare(bin.op, cmp, c32(0), types.int_type(32, true));
    }
    release_operand(l);
    return result;
}

llvm::Value* CodeGen::emit_binary(Expr& e, BinaryExpr& bin) {
    if (bin.op == TokenType::AND_AND || bin.op == TokenType::OR_OR) return emit_logical(bin);
    const Type* ot = bin.operand_type;
    if (ot->kind == TypeKind::String) return emit_string_binary(bin);
    llvm::Value* l = emit_as(*bin.lhs, ot, false);
    llvm::Value* r = emit_as(*bin.rhs, ot, false);
    if (is_comparison(bin.op)) return emit_compare(bin.op, l, r, ot);
    return emit_binop(bin.op, l, r, ot, e.loc.line);
}

llvm::Value* CodeGen::emit_assign(Expr& e, AssignExpr& a) {
    const Type* tt = a.target->type;
    if (a.op == TokenType::EQUAL) {
        // the new value first (an owned copy), then the target's address; the old value is
        // freed only after both are known
        llvm::Value* v = emit_as(*a.value, tt, true);
        llvm::Value* p = emit_place(*a.target, true);
        emit_free(p, tt);
        b.CreateStore(v, p);
        return v;
    }
    if (tt->kind == TypeKind::String) { // s += t / s += c: append in place
        if (a.value->type->kind == TypeKind::Char) {
            llvm::Value* c = emit_expr(*a.value);
            llvm::Value* p = emit_place(*a.target, true);
            b.CreateStore(c, b.CreateCall(rt("vulp_vec_push_slot"), {p, c64(1)}));
            return b.CreateLoad(vec_ty, p);
        }
        Operand r = emit_operand(*a.value);
        // the target's index expressions run next and could overwrite a borrowed right side
        if (has_side_effects(*a.target)) own_operand(r, *a.value);
        llvm::Value* rp = r.slot ? r.slot : spill(r.value, "rhs");
        llvm::Value* p = emit_place(*a.target, true);
        b.CreateCall(rt("vulp_str_append"), {p, rp});
        release_operand(r);
        return b.CreateLoad(vec_ty, p);
    }
    const Type* ot = a.operand_type;
    llvm::Value* r = emit_as(*a.value, ot, false);
    llvm::Value* p = emit_place(*a.target, true);
    llvm::Value* cur = convert(b.CreateLoad(lty(tt), p), tt, ot);
    llvm::Value* result = convert(emit_binop(compound_to_binary(a.op), cur, r, ot, e.loc.line), ot, tt);
    b.CreateStore(result, p);
    return result;
}

llvm::Value* CodeGen::emit_incdec(IncDecExpr& x) {
    llvm::Value* p = emit_place(*x.target, true);
    llvm::Type* t = lty(x.target->type);
    llvm::Value* old = b.CreateLoad(t, p);
    llvm::Value* one = llvm::ConstantInt::get(t, 1);
    b.CreateStore(x.op == TokenType::PLUS_PLUS ? b.CreateAdd(old, one) : b.CreateSub(old, one), p);
    return old;
}

llvm::Value* CodeGen::emit_call(Expr& e, CallExpr& c) {
    switch (c.kind) {
        case CallKind::Builtin:
            return emit_builtin(e, c);
        case CallKind::StructCtor: {
            llvm::StructType* st = struct_types.at(c.st);
            llvm::AllocaInst* tmp = entry_alloca(st, "ctor");
            b.CreateStore(llvm::Constant::getNullValue(st), tmp);
            for (size_t i = 0; i < c.args.size(); i++) {
                llvm::Value* v = emit_as(*c.args[i], c.st->fields[i].type, true);
                b.CreateStore(v, b.CreateStructGEP(st, tmp, static_cast<unsigned>(i)));
            }
            return b.CreateLoad(st, tmp);
        }
        case CallKind::Function: {
            // By-value arguments are owned copies that the callee frees; `&` arguments are
            // addresses. The index / key values of `&` arguments are evaluated first, in order,
            // but their addresses are only taken after every by-value argument has been
            // computed, so evaluating an argument can never leave an address dangling.
            const size_t n = c.args.size();
            std::vector<std::optional<FrozenPlace>> refs(n);
            for (size_t i = 0; i < n; i++) {
                if (c.fn->params[i]->is_ref) refs[i] = freeze_place(*c.args[i]);
            }
            std::vector<llvm::Value*> args(n);
            for (size_t i = 0; i < n; i++) {
                if (!refs[i]) args[i] = emit_as(*c.args[i], c.fn->params[i]->type, true);
            }
            for (size_t i = 0; i < n; i++) {
                if (refs[i]) args[i] = frozen_address(*refs[i], true);
            }
            llvm::Value* result = b.CreateCall(fn_map.at(c.fn), args);
            for (const auto& ref : refs) {
                if (ref) release_frozen(*ref);
            }
            return result;
        }
        case CallKind::Unresolved:
            break;
    }
    throw CodeGenError("unresolved call to " + c.callee);
}

llvm::Value* CodeGen::emit_builtin(Expr& e, CallExpr& c) {
    switch (c.builtin) {
        case Builtin::Exit: {
            llvm::Value* code = emit_as(*c.args[0], types.int_type(32, true), false);
            b.CreateCall(rt("vulp_rt_exit"), {code});
            b.CreateUnreachable();
            start_dead_block();
            return nullptr;
        }
        case Builtin::Abs: {
            llvm::Value* v = emit_as(*c.args[0], e.type, false);
            if (e.type->kind == TypeKind::Float) return b.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, v);
            if (!is_signed_int(e.type)) return v;
            return b.CreateSelect(b.CreateICmpSLT(v, llvm::ConstantInt::get(v->getType(), 0)), b.CreateNeg(v), v);
        }
        case Builtin::Min:
        case Builtin::Max: {
            const bool is_min = c.builtin == Builtin::Min;
            llvm::Value* acc = emit_as(*c.args[0], e.type, false);
            for (size_t i = 1; i < c.args.size(); i++) {
                llvm::Value* v = emit_as(*c.args[i], e.type, false);
                llvm::Value* less = emit_compare(TokenType::LESS, v, acc, e.type);
                acc = is_min ? b.CreateSelect(less, v, acc) : b.CreateSelect(less, acc, v);
            }
            return acc;
        }
        case Builtin::None:
            break;
    }
    throw CodeGenError("unknown builtin");
}

llvm::Value* CodeGen::emit_cast(Expr& e, CastExpr& c) {
    const Type* from = c.operand->type;
    const Type* to = e.type;
    if (to->kind == TypeKind::String) {
        if (from->kind == TypeKind::String) return emit_owned(*c.operand);
        llvm::AllocaInst* dst = entry_alloca(vec_ty, "tostr");
        llvm::Value* v = emit_expr(*c.operand);
        if (from->kind == TypeKind::Char) {
            b.CreateCall(rt("vulp_str_from_bytes"), {dst, spill(v, "ch"), c64(1)});
        } else if (from->kind == TypeKind::Bool) {
            b.CreateCall(rt("vulp_str_from_bytes"), {dst, b.CreateSelect(v, string_constant("true"),
                                                                            string_constant("false")),
                                                     b.CreateSelect(v, c64(4), c64(5))});
        } else if (from->kind == TypeKind::Float) {
            b.CreateCall(rt("vulp_str_from_f64"), {dst, v});
        } else if (from->bits > 64) {
            const std::string fn = std::string("vulp_str_from_") + (from->is_signed ? "i" : "u")
                                   + std::to_string(from->bits);
            b.CreateCall(rt(fn), {dst, spill(v, "wide")});
        } else if (is_signed_int(from)) {
            b.CreateCall(rt("vulp_str_from_i64"), {dst, b.CreateSExtOrTrunc(v, i64())});
        } else {
            b.CreateCall(rt("vulp_str_from_u64"), {dst, b.CreateZExtOrTrunc(v, i64())});
        }
        return b.CreateLoad(vec_ty, dst);
    }
    llvm::Value* v = emit_expr(*c.operand);
    if (to->kind == TypeKind::Bool) {
        if (from->kind == TypeKind::Bool) return v;
        return b.CreateICmpNE(v, llvm::ConstantInt::get(v->getType(), 0));
    }
    if (to->is_integral()) {
        if (from->kind == TypeKind::Bool) return b.CreateZExt(v, lty(to));
        if (from->kind == TypeKind::Float) {
            // truncates toward zero and saturates at the type's limits; NaN becomes 0
            const bool sign = is_signed_int(to);
            const unsigned bits = lty(to)->getIntegerBitWidth();
            if (bits <= 128) {
                const auto id = sign ? llvm::Intrinsic::fptosi_sat : llvm::Intrinsic::fptoui_sat;
                return b.CreateIntrinsic(id, {lty(to), v->getType()}, {v});
            }
            // LLVM cannot lower the saturating intrinsics beyond 128 bits: clamp by hand and
            // only convert values that are in range.
            llvm::Type* dbl = v->getType();
            llvm::Value* limit = llvm::ConstantFP::get(dbl, std::ldexp(1.0, static_cast<int>(sign ? bits - 1 : bits)));
            llvm::Value* is_nan = b.CreateFCmpUNO(v, v);
            llvm::Value* too_big = b.CreateFCmpOGE(v, limit);
            llvm::Value* too_small = sign ? b.CreateFCmpOLT(v, b.CreateFNeg(limit))
                                          : b.CreateFCmpOLT(v, llvm::ConstantFP::get(dbl, 0.0));
            llvm::Value* bad = b.CreateOr(is_nan, b.CreateOr(too_big, too_small));
            llvm::Value* safe = b.CreateSelect(bad, llvm::ConstantFP::get(dbl, 0.0), v);
            llvm::Value* conv = sign ? b.CreateFPToSI(safe, lty(to)) : b.CreateFPToUI(safe, lty(to));
            llvm::Value* max = llvm::ConstantInt::get(ctx, sign ? llvm::APInt::getSignedMaxValue(bits)
                                                                : llvm::APInt::getMaxValue(bits));
            llvm::Value* min = llvm::ConstantInt::get(ctx, sign ? llvm::APInt::getSignedMinValue(bits)
                                                                : llvm::APInt::getZero(bits));
            llvm::Value* r = b.CreateSelect(too_big, max, b.CreateSelect(too_small, min, conv));
            return b.CreateSelect(is_nan, llvm::ConstantInt::get(lty(to), 0), r);
        }
    }
    return convert(v, from, to);
}

llvm::Value* CodeGen::emit_method(Expr& e, MethodCallExpr& m) {
    const Type* bt = m.base->type;
    const int line = e.loc.line;
    switch (m.resolved) {
        case Method::Len: {
            if (!is_place(*m.base)) { // e.g. f().len() or f().v.len(): free the temporary afterwards
                return with_temp_place(*m.base, [&](llvm::Value* p, bool) -> llvm::Value* {
                    if (bt->kind == TypeKind::Map) return b.CreateCall(rt("vulp_map_len"), {b.CreateLoad(ptr(), p)});
                    return seq_len(p);
                });
            }
            llvm::Value* p = emit_place(*m.base, false);
            if (bt->kind == TypeKind::Map) return b.CreateCall(rt("vulp_map_len"), {b.CreateLoad(ptr(), p)});
            return seq_len(p);
        }
        case Method::Push: {
            llvm::Value* v = emit_as(*m.args[0], bt->element(), true);
            llvm::Value* p = emit_place(*m.base, true);
            llvm::Value* slot = b.CreateCall(rt("vulp_vec_push_slot"),
                                             {p, c64(static_cast<int64_t>(size_of(bt->element())))});
            b.CreateStore(v, slot);
            return nullptr;
        }
        case Method::Back:
            if (!is_place(*m.base)) { // back() of a temporary: copy the element out, free the rest
                return with_temp_place(*m.base, [&](llvm::Value* p, const bool owned) -> llvm::Value* {
                    llvm::Value* len = seq_len(p);
                    llvm::Function* fn = b.GetInsertBlock()->getParent();
                    llvm::BasicBlock* empty = llvm::BasicBlock::Create(ctx, "empty", fn);
                    llvm::BasicBlock* ok = llvm::BasicBlock::Create(ctx, "nonempty", fn);
                    b.CreateCondBr(b.CreateICmpEQ(len, c64(0)), empty, ok);
                    b.SetInsertPoint(empty);
                    emit_panic("back() of an empty " + bt->str(), line);
                    b.SetInsertPoint(ok);
                    llvm::Value* last = b.CreateSub(len, c64(1));
                    llvm::Value* v = b.CreateLoad(lty(e.type), b.CreateGEP(lty(e.type), seq_data(p), last));
                    return owned ? deep_copy(v, e.type) : v;
                });
            }
            [[fallthrough]];
        case Method::Pop: {
            const bool pop = m.resolved == Method::Pop;
            llvm::Value* p = emit_place(*m.base, pop);
            llvm::Value* len = seq_len(p);
            llvm::Function* fn = b.GetInsertBlock()->getParent();
            llvm::BasicBlock* empty = llvm::BasicBlock::Create(ctx, "empty", fn);
            llvm::BasicBlock* ok = llvm::BasicBlock::Create(ctx, "nonempty", fn);
            b.CreateCondBr(b.CreateICmpEQ(len, c64(0)), empty, ok);
            b.SetInsertPoint(empty);
            emit_panic(pop ? "pop() from an empty " + bt->str() : "back() of an empty " + bt->str(), line);
            b.SetInsertPoint(ok);
            llvm::Value* last = b.CreateSub(len, c64(1));
            llvm::Value* v = b.CreateLoad(lty(e.type), b.CreateGEP(lty(e.type), seq_data(p), last));
            if (pop) b.CreateStore(last, b.CreateStructGEP(vec_ty, p, 1)); // ownership moves to the caller
            return v;
        }
        case Method::Clear: {
            llvm::Value* p = emit_place(*m.base, true);
            if (bt->kind == TypeKind::Map) {
                llvm::Value* map = b.CreateLoad(ptr(), p);
                emit_free_map_values(map, bt);
                b.CreateCall(rt("vulp_map_clear"), {map});
            } else {
                emit_free_elements(p, bt);
                b.CreateStore(c64(0), b.CreateStructGEP(vec_ty, p, 1)); // keep the capacity
            }
            return nullptr;
        }
        case Method::Has: {
            const Operand key = emit_map_key(*m.args[0], bt, has_side_effects(*m.base)); // key first, then the map
            llvm::Value* map = b.CreateLoad(ptr(), emit_place(*m.base, false));
            llvm::Value* found = map_get_ptr(map, bt, map_key_arg(key, bt));
            release_operand(key);
            return b.CreateIsNotNull(found);
        }
        case Method::Erase: {
            // always own the key: it may live inside the very value that is freed below
            // (`m.erase(m[k].name)`)
            const Operand key = emit_map_key(*m.args[0], bt, true);
            llvm::Value* map = b.CreateLoad(ptr(), emit_place(*m.base, true));
            if (!bt->val->is_trivial()) { // free what the erased value owns
                llvm::Value* found = map_get_ptr(map, bt, map_key_arg(key, bt));
                llvm::AllocaInst* zero = entry_alloca(lty(bt->val), "missing");
                b.CreateStore(llvm::Constant::getNullValue(lty(bt->val)), zero);
                emit_free(b.CreateSelect(b.CreateIsNull(found), zero, found), bt->val);
            }
            const bool str = bt->key->kind == TypeKind::String;
            b.CreateCall(rt(str ? "vulp_map_erase_s" : "vulp_map_erase_i"), {map, map_key_arg(key, bt)});
            release_operand(key);
            return nullptr;
        }
        case Method::Sort:
            b.CreateCall(rt("vulp_sort"), {emit_place(*m.base, true), c32(sort_kind(bt->element()))});
            return nullptr;
        case Method::None:
            break;
    }
    throw CodeGenError("unresolved method " + m.method);
}
