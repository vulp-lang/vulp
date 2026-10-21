#include "codegen/CodeGen.hpp"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

using namespace AST;
using namespace Sema;

CodeGen::CodeGen(Program& p_program, ProgramInfo& p_info, TypeContext& p_types, llvm::LLVMContext& p_ctx,
                 const llvm::DataLayout& p_layout, const std::string& target_triple, CodeGenOptions p_opts)
    : program(p_program), info(p_info), types(p_types), ctx(p_ctx), layout(p_layout),
      module(std::make_unique<llvm::Module>("vulp_module", p_ctx)), b(p_ctx), opts(p_opts) {
    module->setDataLayout(layout);
    module->setTargetTriple(target_triple);
    vec_ty = llvm::StructType::create(ctx, {ptr(), i64(), i64()}, "vulp.vec");
}

std::unique_ptr<llvm::Module> CodeGen::generate() {
    declare_structs();
    declare_globals();
    declare_functions();
    emit_main();
    for (const auto& fn : info.fns) emit_function(fn.get());
    if (opts.emit_c_main) emit_c_main();

    std::string err;
    llvm::raw_string_ostream os(err);
    if (llvm::verifyModule(*module, &os)) {
        os.flush();
        throw CodeGenError("generated invalid LLVM IR:\n" + err);
    }
    return std::move(module);
}

/*** types ***/

llvm::Type* CodeGen::lty(const Type* t) {
    switch (t->kind) {
        case TypeKind::Void: return b.getVoidTy();
        case TypeKind::Bool: return b.getInt1Ty();
        case TypeKind::Int: return b.getIntNTy(static_cast<unsigned>(t->bits));
        case TypeKind::Char: return i8();
        case TypeKind::Float: return b.getDoubleTy();
        case TypeKind::String:
        case TypeKind::Vec:
        case TypeKind::Arr:
            return vec_ty;
        case TypeKind::Map: return ptr();
        case TypeKind::Struct: return struct_types.at(t->st);
        case TypeKind::Error: break;
    }
    throw CodeGenError("cannot lower type " + t->str());
}

uint64_t CodeGen::size_of(const Type* t) {
    return layout.getTypeAllocSize(lty(t)).getFixedValue();
}

void CodeGen::declare_structs() {
    for (const auto& st : info.structs) struct_types[st.get()] = llvm::StructType::create(ctx, "S." + st->name);
    for (const auto& st : info.structs) {
        std::vector<llvm::Type*> fields;
        for (const auto& f : st->fields) fields.push_back(lty(f.type));
        struct_types[st.get()]->setBody(fields);
    }
}

void CodeGen::declare_globals() {
    for (const VarInfo* var : info.globals) {
        llvm::Type* t = lty(var->type);
        auto* g = new llvm::GlobalVariable(*module, t, false, llvm::GlobalValue::InternalLinkage,
                                           llvm::Constant::getNullValue(t), "g." + var->name);
        var_storage[var] = g;
    }
}

/*** runtime declarations ***/

llvm::FunctionCallee CodeGen::rt(const std::string& name) {
    struct Sig {
        const char* name;
        int ret;               // 0 void, 1 i32, 2 i64, 3 ptr, 4 double
        std::vector<int> args; // 1 i32, 2 i64, 3 ptr, 4 double
        bool noreturn;
    };
    static const std::vector<Sig> sigs = {
        {"vulp_rt_start", 1, {3}, false},
        {"vulp_rt_exit", 0, {1}, true},
        {"vulp_rt_panic", 0, {3, 1}, true},
        {"vulp_rt_bounds_fail", 0, {2, 2, 1}, true},
        {"vulp_rt_div_zero", 0, {1}, true},
        {"vulp_rt_free", 0, {3}, false},
        {"vulp_print_i64", 0, {2}, false},
        {"vulp_print_u64", 0, {2}, false},
        {"vulp_print_i128", 0, {3}, false},
        {"vulp_print_u128", 0, {3}, false},
        {"vulp_print_i256", 0, {3}, false},
        {"vulp_print_u256", 0, {3}, false},
        {"vulp_print_f64", 0, {4}, false},
        {"vulp_print_char", 0, {1}, false},
        {"vulp_print_bool", 0, {1}, false},
        {"vulp_print_bytes", 0, {3, 2}, false},
        {"vulp_print_str", 0, {3}, false},
        {"vulp_read_int", 2, {1, 1, 1}, false},
        {"vulp_read_i64", 2, {1}, false},
        {"vulp_read_u64", 2, {1}, false},
        {"vulp_read_i128", 0, {3, 1}, false},
        {"vulp_read_u128", 0, {3, 1}, false},
        {"vulp_read_i256", 0, {3, 1}, false},
        {"vulp_read_u256", 0, {3, 1}, false},
        {"vulp_read_f64", 4, {1}, false},
        {"vulp_read_char", 1, {1}, false},
        {"vulp_read_str", 0, {3, 1}, false},
        {"vulp_vec_alloc", 0, {3, 2, 2, 1}, false},
        {"vulp_vec_push_slot", 3, {3, 2}, false},
        {"vulp_vec_clone", 0, {3, 3, 2}, false},
        {"vulp_str_from_bytes", 0, {3, 3, 2}, false},
        {"vulp_str_concat", 0, {3, 3, 3}, false},
        {"vulp_str_append", 0, {3, 3}, false},
        {"vulp_str_cmp", 1, {3, 3}, false},
        {"vulp_str_from_i64", 0, {3, 2}, false},
        {"vulp_str_from_u64", 0, {3, 2}, false},
        {"vulp_str_from_i128", 0, {3, 3}, false},
        {"vulp_str_from_u128", 0, {3, 3}, false},
        {"vulp_str_from_i256", 0, {3, 3}, false},
        {"vulp_str_from_u256", 0, {3, 3}, false},
        {"vulp_str_from_f64", 0, {3, 4}, false},
        {"vulp_sort", 0, {3, 1}, false},
        {"vulp_map_index_i", 3, {3, 1, 2, 2}, false},
        {"vulp_map_index_s", 3, {3, 2, 3}, false},
        {"vulp_map_get_i", 3, {3, 2}, false},
        {"vulp_map_get_s", 3, {3, 3}, false},
        {"vulp_map_free", 0, {3}, false},
        {"vulp_map_values", 0, {3, 3}, false},
        {"vulp_map_has_i", 1, {3, 2}, false},
        {"vulp_map_has_s", 1, {3, 3}, false},
        {"vulp_map_erase_i", 0, {3, 2}, false},
        {"vulp_map_erase_s", 0, {3, 3}, false},
        {"vulp_map_len", 2, {3}, false},
        {"vulp_map_clear", 0, {3}, false},
        {"vulp_map_clone", 3, {3}, false},
        {"vulp_map_keys", 0, {3, 3}, false},
        {"vulp_memo_get", 3, {3, 2, 2, 3}, false},
        {"vulp_memo_put", 0, {3, 2, 2, 3, 3}, false},
        {"vulp_memo_clear", 0, {3}, false},
    };
    const auto to_type = [&](const int code) -> llvm::Type* {
        switch (code) {
            case 0: return b.getVoidTy();
            case 1: return i32();
            case 2: return i64();
            case 3: return ptr();
            default: return b.getDoubleTy();
        }
    };
    for (const auto& sig : sigs) {
        if (name != sig.name) continue;
        std::vector<llvm::Type*> args;
        for (const int a : sig.args) args.push_back(to_type(a));
        auto* ft = llvm::FunctionType::get(to_type(sig.ret), args, false);
        llvm::FunctionCallee callee = module->getOrInsertFunction(name, ft);
        if (sig.noreturn) {
            if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee())) f->setDoesNotReturn();
        }
        return callee;
    }
    throw CodeGenError("unknown runtime function " + name);
}

/*** small helpers ***/

llvm::AllocaInst* CodeGen::entry_alloca(llvm::Type* t, const std::string& name) {
    llvm::BasicBlock& entry = cur_fn->getEntryBlock();
    llvm::IRBuilder<> tmp(&entry, entry.begin());
    return tmp.CreateAlloca(t, nullptr, name);
}

llvm::AllocaInst* CodeGen::zeroed_alloca(const Type* t, const std::string& name) {
    llvm::AllocaInst* slot = entry_alloca(lty(t), name);
    if (!t->is_trivial()) {
        llvm::Instruction* next = slot->getNextNode();
        llvm::IRBuilder<> tmp = next ? llvm::IRBuilder<>(next) : llvm::IRBuilder<>(slot->getParent());
        tmp.CreateStore(llvm::Constant::getNullValue(lty(t)), slot);
    }
    return slot;
}

llvm::Value* CodeGen::spill(llvm::Value* v, const std::string& name) {
    llvm::AllocaInst* slot = entry_alloca(v->getType(), name);
    b.CreateStore(v, slot);
    return slot;
}

llvm::Constant* CodeGen::string_constant(const std::string& s) {
    if (const auto it = string_constants.find(s); it != string_constants.end()) return it->second;
    llvm::Constant* data = llvm::ConstantDataArray::getString(ctx, s, false);
    auto* g = new llvm::GlobalVariable(*module, data->getType(), true, llvm::GlobalValue::PrivateLinkage, data, ".str");
    g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    string_constants[s] = g;
    return g;
}

// A constant string header pointing at the literal's bytes. Only for read-only uses
// (comparisons, concatenation operands, map keys); stored strings are always heap copies.
llvm::Constant* CodeGen::string_header(const std::string& s) {
    if (const auto it = string_headers.find(s); it != string_headers.end()) return it->second;
    llvm::Constant* hdr = llvm::ConstantStruct::get(
        vec_ty, {string_constant(s), c64(static_cast<int64_t>(s.size())), c64(static_cast<int64_t>(s.size()))});
    auto* g = new llvm::GlobalVariable(*module, vec_ty, true, llvm::GlobalValue::PrivateLinkage, hdr, ".strhdr");
    string_headers[s] = g;
    return g;
}

void CodeGen::start_dead_block() {
    llvm::BasicBlock* dead = llvm::BasicBlock::Create(ctx, "dead", b.GetInsertBlock()->getParent());
    b.SetInsertPoint(dead);
}

void CodeGen::emit_panic(const std::string& msg, const int line) {
    b.CreateCall(rt("vulp_rt_panic"), {string_constant(msg + std::string(1, '\0')), c32(line)});
    b.CreateUnreachable();
}

/*** functions ***/

void CodeGen::declare_functions() {
    for (const auto& fn : info.fns) {
        std::vector<llvm::Type*> params;
        for (const VarInfo* p : fn->params) params.push_back(p->is_ref ? ptr() : lty(p->type));
        auto* ft = llvm::FunctionType::get(lty(fn->ret), params, false);
        auto* f = llvm::Function::Create(ft, llvm::Function::InternalLinkage, "fn." + fn->name, module.get());
        fn_map[fn.get()] = f;
        if (fn->is_dp) {
            auto* impl = llvm::Function::Create(ft, llvm::Function::InternalLinkage, "dp.impl." + fn->name,
                                                module.get());
            dp_impl[fn.get()] = impl;
            dp_memo[fn.get()] = new llvm::GlobalVariable(*module, ptr(), false, llvm::GlobalValue::InternalLinkage,
                                                         llvm::ConstantPointerNull::get(b.getPtrTy()),
                                                         "dp.memo." + fn->name);
        }
    }
}

void CodeGen::emit_function(const FnInfo* fn) {
    llvm::Function* f = fn->is_dp ? dp_impl.at(fn) : fn_map.at(fn);
    cur_fn = f;
    cur_info = fn;
    loops.clear();
    owned_locals.clear();
    b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", f));
    // Every `return` stores its value and jumps to exit_bb, which frees the function's owned
    // locals and by-value parameters.
    ret_slot = fn->ret->kind == TypeKind::Void ? nullptr : entry_alloca(lty(fn->ret), "ret");
    exit_bb = llvm::BasicBlock::Create(ctx, "exit", f);

    size_t i = 0;
    for (llvm::Argument& arg : f->args()) {
        const VarInfo* p = fn->params[i++];
        arg.setName(p->name);
        if (p->is_ref) {
            var_storage[p] = &arg;
        } else {
            llvm::AllocaInst* slot = entry_alloca(arg.getType(), p->name);
            b.CreateStore(&arg, slot);
            var_storage[p] = slot;
            if (!p->type->is_trivial()) owned_locals.emplace_back(slot, p->type);
        }
    }

    emit_stmt(*fn->decl->body);

    if (!block_terminated()) {
        if (fn->ret->kind == TypeKind::Void) b.CreateBr(exit_bb);
        else b.CreateUnreachable(); // sema proved every path returns
    }
    b.SetInsertPoint(exit_bb);
    for (const auto& [slot, type] : owned_locals) emit_free(slot, type);
    if (ret_slot) b.CreateRet(b.CreateLoad(lty(fn->ret), ret_slot));
    else b.CreateRetVoid();
    ret_slot = nullptr;
    exit_bb = nullptr;
    if (fn->is_dp) emit_dp_wrapper(fn);
}

// The callable entry of a dp function: look the arguments up in the memo table, otherwise
// call the implementation and remember its result.
void CodeGen::emit_dp_wrapper(const FnInfo* fn) {
    llvm::Function* f = fn_map.at(fn);
    llvm::Function* impl = dp_impl.at(fn);
    llvm::GlobalVariable* memo = dp_memo.at(fn);
    cur_fn = f;
    b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", f));

    // key = the arguments packed bytewise (bools as one byte), padding zeroed
    uint64_t key_size = 0;
    std::vector<uint64_t> offsets;
    for (const VarInfo* p : fn->params) {
        offsets.push_back(key_size);
        key_size += size_of(p->type);
    }
    if (key_size == 0) key_size = 1;
    llvm::AllocaInst* key = entry_alloca(llvm::ArrayType::get(i8(), key_size), "memo.key");
    b.CreateMemSet(key, b.getInt8(0), key_size, llvm::MaybeAlign(1));
    std::vector<llvm::Value*> args;
    size_t i = 0;
    for (llvm::Argument& arg : f->args()) {
        args.push_back(&arg);
        llvm::Value* slot = b.CreateConstGEP1_64(i8(), key, offsets[i]);
        llvm::Value* v = &arg;
        if (fn->params[i]->type->kind == TypeKind::Bool) v = b.CreateZExt(v, i8());
        b.CreateAlignedStore(v, slot, llvm::MaybeAlign(1));
        i++;
    }

    const uint64_t val_size = size_of(fn->ret);
    llvm::Value* found = b.CreateCall(rt("vulp_memo_get"), {memo, c64(static_cast<int64_t>(key_size)),
                                                            c64(static_cast<int64_t>(val_size)), key});
    llvm::BasicBlock* hit = llvm::BasicBlock::Create(ctx, "memo.hit", f);
    llvm::BasicBlock* miss = llvm::BasicBlock::Create(ctx, "memo.miss", f);
    b.CreateCondBr(b.CreateIsNotNull(found), hit, miss);

    b.SetInsertPoint(hit);
    b.CreateRet(b.CreateAlignedLoad(lty(fn->ret), found, llvm::MaybeAlign(1)));

    b.SetInsertPoint(miss);
    llvm::Value* result = b.CreateCall(impl, args);
    llvm::Value* result_slot = spill(result, "memo.val");
    b.CreateCall(rt("vulp_memo_put"), {memo, c64(static_cast<int64_t>(key_size)), c64(static_cast<int64_t>(val_size)),
                                       key, result_slot});
    b.CreateRet(result);
}

void CodeGen::emit_main() {
    auto* ft = llvm::FunctionType::get(i32(), false);
    llvm::Function* f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "vulp_main", module.get());
    cur_fn = f;
    cur_info = nullptr;
    loops.clear();
    owned_locals.clear();
    b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", f));
    for (auto& item : program.items) {
        if (std::holds_alternative<FnDecl>(item->node) || std::holds_alternative<StructDecl>(item->node)) continue;
        if (block_terminated()) break; // everything after a top-level exit() is dead
        emit_stmt(*item);
    }
    if (!block_terminated()) b.CreateRet(c32(0));
}

void CodeGen::emit_c_main() {
    auto* ft = llvm::FunctionType::get(i32(), {i32(), ptr()}, false);
    llvm::Function* f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "main", module.get());
    cur_fn = f;
    b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", f));
    llvm::Value* code = b.CreateCall(rt("vulp_rt_start"), {module->getFunction("vulp_main")});
    b.CreateRet(code);
}
