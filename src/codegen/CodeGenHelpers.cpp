#include "codegen/CodeGen.hpp"

#include "vulp_runtime.h"

using namespace AST;
using namespace Sema;

/*** implicit / explicit conversions ***/

llvm::Value* CodeGen::convert(llvm::Value* v, const Type* from, const Type* to) {
    if (from == to) return v;
    if (from->is_sequence() && to->is_sequence()) return v; // vec<T> <-> T[]: same layout
    if (from->is_integral() && to->is_integral()) {
        const bool sign = from->kind == TypeKind::Int && from->is_signed;
        return sign ? b.CreateSExtOrTrunc(v, lty(to)) : b.CreateZExtOrTrunc(v, lty(to));
    }
    if (from->is_integral() && to->kind == TypeKind::Float) {
        const bool sign = from->kind == TypeKind::Int && from->is_signed;
        return sign ? b.CreateSIToFP(v, lty(to)) : b.CreateUIToFP(v, lty(to));
    }
    throw CodeGenError("cannot convert " + from->str() + " to " + to->str());
}

llvm::Value* CodeGen::to_i64(llvm::Value* v, const Type* t) {
    if (t->kind == TypeKind::Int && t->is_signed) return b.CreateSExtOrTrunc(v, i64());
    return b.CreateZExtOrTrunc(v, i64());
}

void CodeGen::emit_print_bytes(const std::string& s) {
    if (s.empty()) return;
    b.CreateCall(rt("vulp_print_bytes"), {string_constant(s), c64(static_cast<int64_t>(s.size()))});
}

int32_t CodeGen::sort_kind(const Type* elem) {
    switch (elem->kind) {
        case TypeKind::Char:
        case TypeKind::Bool:
            return VULP_SORT_U8;
        case TypeKind::Float:
            return VULP_SORT_F64;
        case TypeKind::String:
            return VULP_SORT_STR;
        case TypeKind::Int: {
            const int base = elem->is_signed ? VULP_SORT_I8 : VULP_SORT_U8;
            switch (elem->bits) {
                case 8: return base;
                case 16: return base + 1;
                case 32: return base + 2;
                case 64: return base + 3;
                case 128: return base + 4;
                default: break;
            }
            break;
        }
        default:
            break;
    }
    throw CodeGenError("cannot sort elements of type " + elem->str());
}

/*** deep copies ***/

// A "fix" helper makes the value at `p` own fresh copies of every heap buffer reachable from
// it. Copying a value = bitwise copy + fix. Fix helpers read the old buffers before
// overwriting the header, so fixing in place is safe.
llvm::Function* CodeGen::fix_fn(const Type* t) {
    return make_helper(fix_fns, t, "fix.", {ptr()}, [&](llvm::Function* f) {
        llvm::Value* p = f->getArg(0);
        switch (t->kind) {
            case TypeKind::String:
            case TypeKind::Vec:
            case TypeKind::Arr: {
                const Type* elem = t->element();
                b.CreateCall(rt("vulp_vec_clone"), {p, p, c64(static_cast<int64_t>(size_of(elem)))});
                if (!elem->is_trivial()) {
                    llvm::Value* data = seq_data(p);
                    emit_counted_loop(seq_len(p), [&](llvm::Value* i) {
                        emit_fix(b.CreateGEP(lty(elem), data, i), elem);
                    });
                }
                break;
            }
            case TypeKind::Map: {
                llvm::Value* copy = b.CreateCall(rt("vulp_map_clone"), {b.CreateLoad(ptr(), p)});
                b.CreateStore(copy, p);
                if (!t->val->is_trivial()) {
                    llvm::AllocaInst* values = entry_alloca(vec_ty, "values");
                    b.CreateCall(rt("vulp_map_values"), {copy, values});
                    llvm::Value* data = seq_data(values);
                    emit_counted_loop(seq_len(values), [&](llvm::Value* i) {
                        emit_fix(b.CreateLoad(ptr(), b.CreateGEP(ptr(), data, i)), t->val);
                    });
                    b.CreateCall(rt("vulp_rt_free"), {data});
                }
                break;
            }
            case TypeKind::Struct: {
                llvm::StructType* st = struct_types.at(t->st);
                for (size_t i = 0; i < t->st->fields.size(); i++) {
                    const Type* ft_i = t->st->fields[i].type;
                    if (!ft_i->is_trivial()) emit_fix(b.CreateStructGEP(st, p, static_cast<unsigned>(i)), ft_i);
                }
                break;
            }
            default:
                break;
        }
    });
}

void CodeGen::emit_fix(llvm::Value* p, const Type* t) {
    if (!t->is_trivial()) b.CreateCall(fix_fn(t), {p});
}

llvm::Value* CodeGen::deep_copy(llvm::Value* v, const Type* t) {
    if (t->is_trivial()) return v;
    llvm::Value* slot = spill(v, "copy");
    emit_fix(slot, t);
    return b.CreateLoad(lty(t), slot);
}

/*** frees ***/

// A "free" helper releases every heap buffer owned by the value at `p` (the value itself is
// left as it is; callers overwrite it). An all-zero value owns nothing.
llvm::Function* CodeGen::free_fn(const Type* t) {
    return make_helper(free_fns, t, "free.", {ptr()}, [&](llvm::Function* f) {
        llvm::Value* p = f->getArg(0);
        switch (t->kind) {
            case TypeKind::String:
            case TypeKind::Vec:
            case TypeKind::Arr:
                emit_free_elements(p, t);
                b.CreateCall(rt("vulp_rt_free"), {seq_data(p)});
                break;
            case TypeKind::Map: {
                llvm::Value* map = b.CreateLoad(ptr(), p);
                emit_free_map_values(map, t);
                b.CreateCall(rt("vulp_map_free"), {map});
                break;
            }
            case TypeKind::Struct: {
                llvm::StructType* st = struct_types.at(t->st);
                for (size_t i = 0; i < t->st->fields.size(); i++) {
                    const Type* ft_i = t->st->fields[i].type;
                    if (!ft_i->is_trivial()) emit_free(b.CreateStructGEP(st, p, static_cast<unsigned>(i)), ft_i);
                }
                break;
            }
            default:
                break;
        }
    });
}

void CodeGen::emit_free(llvm::Value* p, const Type* t) {
    if (!t->is_trivial()) b.CreateCall(free_fn(t), {p});
}

void CodeGen::emit_free_elements(llvm::Value* hdr, const Type* seq_t) {
    const Type* elem = seq_t->element();
    if (elem->is_trivial()) return;
    llvm::Value* data = seq_data(hdr);
    emit_counted_loop(seq_len(hdr), [&](llvm::Value* i) { emit_free(b.CreateGEP(lty(elem), data, i), elem); });
}

void CodeGen::emit_free_map_values(llvm::Value* map, const Type* map_t) {
    if (map_t->val->is_trivial()) return;
    llvm::AllocaInst* values = entry_alloca(vec_ty, "values");
    b.CreateCall(rt("vulp_map_values"), {map, values});
    llvm::Value* data = seq_data(values);
    emit_counted_loop(seq_len(values), [&](llvm::Value* i) {
        emit_free(b.CreateLoad(ptr(), b.CreateGEP(ptr(), data, i)), map_t->val);
    });
    b.CreateCall(rt("vulp_rt_free"), {data});
}

/*** printing ***/

void CodeGen::emit_print_value(llvm::Value* v, const Type* t) {
    switch (t->kind) {
        case TypeKind::Bool:
            b.CreateCall(rt("vulp_print_bool"), {b.CreateZExt(v, i32())});
            return;
        case TypeKind::Char:
            b.CreateCall(rt("vulp_print_char"), {b.CreateZExt(v, i32())});
            return;
        case TypeKind::Float:
            b.CreateCall(rt("vulp_print_f64"), {v});
            return;
        case TypeKind::Int:
            if (t->bits <= 64) {
                if (t->is_signed) b.CreateCall(rt("vulp_print_i64"), {b.CreateSExt(v, i64())});
                else b.CreateCall(rt("vulp_print_u64"), {b.CreateZExtOrTrunc(v, i64())});
                return;
            }
            [[fallthrough]];
        default:
            emit_print_at(spill(v, "print"), t);
    }
}

void CodeGen::emit_print_at(llvm::Value* p, const Type* t) {
    switch (t->kind) {
        case TypeKind::String:
            b.CreateCall(rt("vulp_print_str"), {p});
            return;
        case TypeKind::Vec:
        case TypeKind::Arr:
        case TypeKind::Struct:
            b.CreateCall(print_fn(t), {p});
            return;
        case TypeKind::Int:
            if (t->bits == 128) {
                b.CreateCall(rt(t->is_signed ? "vulp_print_i128" : "vulp_print_u128"), {p});
                return;
            }
            if (t->bits == 256) {
                b.CreateCall(rt(t->is_signed ? "vulp_print_i256" : "vulp_print_u256"), {p});
                return;
            }
            [[fallthrough]];
        default:
            emit_print_value(b.CreateLoad(lty(t), p), t);
    }
}

// Arrays print their elements separated by spaces (rows of nested arrays and structs by
// newlines); structs print their fields separated by spaces.
llvm::Function* CodeGen::print_fn(const Type* t) {
    return make_helper(print_fns, t, "print.", {ptr()}, [&](llvm::Function* f) {
        llvm::Value* p = f->getArg(0);
        if (t->kind == TypeKind::Struct) {
            llvm::StructType* st = struct_types.at(t->st);
            for (size_t i = 0; i < t->st->fields.size(); i++) {
                if (i > 0) emit_print_bytes(" ");
                emit_print_at(b.CreateStructGEP(st, p, static_cast<unsigned>(i)), t->st->fields[i].type);
            }
            return;
        }
        const Type* elem = t->element();
        const bool rows = elem->kind == TypeKind::Vec || elem->kind == TypeKind::Arr || elem->kind == TypeKind::Struct;
        llvm::Value* data = seq_data(p);
        emit_counted_loop(seq_len(p), [&](llvm::Value* i) {
            llvm::BasicBlock* sep = llvm::BasicBlock::Create(ctx, "sep", f);
            llvm::BasicBlock* item = llvm::BasicBlock::Create(ctx, "item", f);
            b.CreateCondBr(b.CreateICmpSGT(i, c64(0)), sep, item);
            b.SetInsertPoint(sep);
            emit_print_bytes(rows ? "\n" : " ");
            b.CreateBr(item);
            b.SetInsertPoint(item);
            emit_print_at(b.CreateGEP(lty(elem), data, i), elem);
        });
    });
}

/*** reading ***/

void CodeGen::emit_read_into(llvm::Value* p, const Type* t, llvm::Value* line) {
    switch (t->kind) {
        case TypeKind::Int:
            if (t->bits <= 64) { // the runtime rejects values that do not fit the target type
                llvm::Value* v = b.CreateCall(rt("vulp_read_int"), {c32(t->bits), c32(t->is_signed ? 1 : 0), line});
                b.CreateStore(b.CreateTrunc(v, lty(t)), p);
            } else if (t->bits == 128) {
                b.CreateCall(rt(t->is_signed ? "vulp_read_i128" : "vulp_read_u128"), {p, line});
            } else {
                b.CreateCall(rt(t->is_signed ? "vulp_read_i256" : "vulp_read_u256"), {p, line});
            }
            return;
        case TypeKind::Char:
            b.CreateStore(b.CreateTrunc(b.CreateCall(rt("vulp_read_char"), {line}), i8()), p);
            return;
        case TypeKind::Float:
            b.CreateStore(b.CreateCall(rt("vulp_read_f64"), {line}), p);
            return;
        case TypeKind::String:
            b.CreateCall(rt("vulp_read_str"), {p, line});
            return;
        case TypeKind::Vec:
        case TypeKind::Arr:
        case TypeKind::Struct:
            b.CreateCall(read_fn(t), {p, line});
            return;
        default:
            throw CodeGenError("cannot read a value of type " + t->str());
    }
}

llvm::Function* CodeGen::read_fn(const Type* t) {
    return make_helper(read_fns, t, "read.", {ptr(), i32()}, [&](llvm::Function* f) {
        llvm::Value* p = f->getArg(0);
        llvm::Value* line = f->getArg(1);
        if (t->kind == TypeKind::Struct) {
            llvm::StructType* st = struct_types.at(t->st);
            for (size_t i = 0; i < t->st->fields.size(); i++) {
                emit_read_into(b.CreateStructGEP(st, p, static_cast<unsigned>(i)), t->st->fields[i].type, line);
            }
            return;
        }
        const Type* elem = t->element();
        llvm::Value* data = seq_data(p);
        emit_counted_loop(seq_len(p), [&](llvm::Value* i) {
            emit_read_into(b.CreateGEP(lty(elem), data, i), elem, line);
        });
    });
}

/*** array allocation ***/

void CodeGen::emit_alloc_dims(llvm::Value* p, const Type* t, const std::vector<llvm::Value*>& sizes,
                              const size_t level, const int line) {
    const Type* elem = t->element();
    b.CreateCall(rt("vulp_vec_alloc"), {p, sizes[level], c64(static_cast<int64_t>(size_of(elem))), c32(line)});
    if (level + 1 < sizes.size()) {
        llvm::Value* data = seq_data(p);
        emit_counted_loop(sizes[level], [&](llvm::Value* i) {
            emit_alloc_dims(b.CreateGEP(lty(elem), data, i), elem, sizes, level + 1, line);
        });
    }
}
