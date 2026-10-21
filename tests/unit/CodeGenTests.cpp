#include "TestFramework.hpp"

#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include "codegen/CodeGen.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/Semantics.hpp"
#include "vulp_runtime.h"

namespace {
    struct Compiled {
        std::unique_ptr<llvm::LLVMContext> ctx;
        std::unique_ptr<llvm::Module> module;
        std::string errors;
    };

    void init_llvm() {
        static const bool once = [] {
            llvm::InitializeNativeTarget();
            llvm::InitializeNativeTargetAsmPrinter();
            llvm::InitializeNativeTargetAsmParser();
            return true;
        }();
        (void)once;
    }

    Compiled compile(const std::string& src, CodeGenOptions opts = {}) {
        init_llvm();
        Compiled out;
        out.ctx = std::make_unique<llvm::LLVMContext>();
        Diagnostics diag("test.vp", src);
        auto tokens = Lexer::tokenize(src, diag);
        Parser parser(std::move(tokens), diag);
        parser.parse();
        AST::Program program = parser.take_program();
        Sema::TypeContext types;
        SemanticAnalyzer sema(program, diag, types);
        if (!diag.has_errors()) sema.analyze();
        if (diag.has_errors()) {
            out.errors = diag.format();
            return out;
        }
        auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
        auto layout = jtmb->getDefaultDataLayoutForTarget();
        try {
            CodeGen cg(program, sema.program_info(), types, *out.ctx, *layout, jtmb->getTargetTriple().str(), opts);
            out.module = cg.generate();
        } catch (const CodeGenError& e) {
            out.errors = e.what();
        }
        return out;
    }

    std::string ir(const std::string& src, const CodeGenOptions opts = {}) {
        Compiled c = compile(src, opts);
        if (!c.module) return "ERROR: " + c.errors;
        std::string text;
        llvm::raw_string_ostream os(text);
        c.module->print(os, nullptr);
        return os.str();
    }

    // Compiles `src`, runs it in-process with the LLVM JIT and returns its stdout.
    std::string run(const std::string& src, const std::string& input = "") {
        Compiled c = compile(src);
        if (!c.module) return "ERROR: " + c.errors;
        auto jit = llvm::orc::LLJITBuilder().create();
        if (!jit) return "ERROR: " + llvm::toString(jit.takeError());
        auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            (*jit)->getDataLayout().getGlobalPrefix());
        (*jit)->getMainJITDylib().addGenerator(std::move(*gen));
        if (auto e = (*jit)->addIRModule(llvm::orc::ThreadSafeModule(std::move(c.module), std::move(c.ctx)))) {
            return "ERROR: " + llvm::toString(std::move(e));
        }
        auto sym = (*jit)->lookup("vulp_main");
        if (!sym) return "ERROR: " + llvm::toString(sym.takeError());
        vulp_rt_test_set_input(input);
        vulp_rt_test_capture_begin();
        vulp_rt_start(sym->toPtr<int32_t (*)()>());
        return vulp_rt_test_capture_end();
    }
} // namespace

// ---- IR structure ----

TEST_CASE("codegen: top-level declarations become globals, function locals are allocas") {
    const std::string text = ir("int x = 5; fn f() -> int { int y = x; return y; }");
    CHECK_CONTAINS(text, "@g.x = internal global i64 0");
    CHECK_CONTAINS(text, "define i32 @vulp_main()");
    CHECK_CONTAINS(text, "define internal i64 @fn.f()");
    CHECK_CONTAINS(text, "%y = alloca i64");
}

TEST_CASE("codegen: integer widths are rounded to powers of two") {
    const std::string text = ir("i21 a; int<0..300> b; u3 c; i200 d; int<-1..1000000000000> e;");
    CHECK_CONTAINS(text, "@g.a = internal global i32 0");
    CHECK_CONTAINS(text, "@g.b = internal global i16 0");
    CHECK_CONTAINS(text, "@g.c = internal global i8 0");
    CHECK_CONTAINS(text, "@g.d = internal global i256 0");
    CHECK_CONTAINS(text, "@g.e = internal global i64 0");
}

TEST_CASE("codegen: containers and structs have the documented layout") {
    const std::string text = ir("struct P { int x; vec<int> v; char c; } P p; map<int, int> m; string s;");
    CHECK_CONTAINS(text, "%vulp.vec = type { ptr, i64, i64 }");
    CHECK_CONTAINS(text, "%S.P = type { i64, %vulp.vec, i8 }");
    CHECK_CONTAINS(text, "@g.m = internal global ptr null");
    CHECK_CONTAINS(text, "@g.s = internal global %vulp.vec zeroinitializer");
}

TEST_CASE("codegen: dp functions get a memo table, a cached wrapper and an implementation") {
    const std::string text = ir("dp fn fib(i32 n) -> i32 { if n <= 1: return n; return fib(n - 1) + fib(n - 2); }");
    CHECK_CONTAINS(text, "@dp.memo.fib = internal global ptr null");
    CHECK_CONTAINS(text, "define internal i32 @fn.fib(i32");
    CHECK_CONTAINS(text, "define internal i32 @dp.impl.fib(i32");
    CHECK_CONTAINS(text, "call ptr @vulp_memo_get");
    CHECK_CONTAINS(text, "call void @vulp_memo_put");
}

TEST_CASE("codegen: TC clears only the caches of dp functions that read globals") {
    const std::string pure = ir("dp fn f(int n) -> int { return n; } TC: { println(f(1)); }");
    CHECK_CONTAINS(pure, "call i64 @vulp_read_i64");
    CHECK(pure.find("vulp_memo_clear") == std::string::npos);
    const std::string reads = ir("int k; dp fn f(int n) -> int { return n + k; } TC: { println(f(1)); }");
    CHECK_CONTAINS(reads, "call void @vulp_memo_clear");
}

TEST_CASE("codegen: copying an owning value calls a deep-copy helper") {
    const std::string text = ir("vec<vec<int>> a; vec<vec<int>> b = a;");
    CHECK_CONTAINS(text, "define internal void @fix.vec.vec.i64(ptr");
    CHECK_CONTAINS(text, "call void @fix.vec.i64(");
    // trivially copyable values are copied with plain loads/stores
    CHECK(ir("int[] a; int x = a[0];").find("@fix.") == std::string::npos);
}

TEST_CASE("codegen: map reads look up without inserting, writes insert") {
    const std::string text = ir("map<int, int> m; int x = m[3]; m[4] = x;");
    CHECK_CONTAINS(text, "call ptr @vulp_map_get_i");
    CHECK_CONTAINS(text, "call ptr @vulp_map_index_i");
}

TEST_CASE("codegen: owned values are freed when overwritten and at function exit") {
    const std::string text = ir("fn f(vec<int> a) -> int { vec<int> b = a; return b.len(); } "
                                "vec<int> g; g = [1]; println(f(g));");
    CHECK_CONTAINS(text, "define internal void @free.vec.i64(ptr");
    CHECK_CONTAINS(text, "call void @vulp_rt_free");
    // f frees its parameter and its local on the way out
    const size_t fn_start = text.find("define internal i64 @fn.f(");
    const size_t fn_end = text.find("\n}\n", fn_start);
    const std::string body = text.substr(fn_start, fn_end - fn_start);
    size_t frees = 0;
    for (size_t pos = body.find("call void @free.vec.i64"); pos != std::string::npos;
         pos = body.find("call void @free.vec.i64", pos + 1)) {
        frees++;
    }
    CHECK(frees >= 2);
}

TEST_CASE("codegen(jit): integer corner cases are defined") {
    CHECK_EQ(run("int mn = -9223372036854775807 - 1; int sh = 70; println(mn / -1, \" \", mn % -1, \" \", 1 << sh);"),
             std::string("-9223372036854775808 0 0\n"));
    CHECK_EQ(run("println(int(1e30), \" \", int(0.0 / 0.0));"), std::string("9223372036854775807 0\n"));
}

TEST_CASE("codegen: runtime checks can be disabled") {
    const std::string src = "int[3] a; int i = 1; int d = 0; println(a[i], 7 / d);";
    const std::string checked = ir(src);
    CHECK_CONTAINS(checked, "vulp_rt_bounds_fail");
    CHECK_CONTAINS(checked, "vulp_rt_div_zero");
    CodeGenOptions opts;
    opts.runtime_checks = false;
    const std::string unchecked = ir(src, opts);
    CHECK(unchecked.find("vulp_rt_bounds_fail") == std::string::npos);
    CHECK(unchecked.find("vulp_rt_div_zero") == std::string::npos);
}

TEST_CASE("codegen: executables get a C main that starts the runtime") {
    CodeGenOptions opts;
    opts.emit_c_main = true;
    CHECK_CONTAINS(ir("println(1);", opts), "call i32 @vulp_rt_start(ptr @vulp_main)");
    CHECK(ir("println(1);").find("@main(") == std::string::npos);
}

TEST_CASE("codegen: string literals in print are not heap allocated") {
    const std::string text = ir("println(\"hi\");");
    CHECK_CONTAINS(text, "call void @vulp_print_bytes");
    CHECK(text.find("vulp_str_from_bytes") == std::string::npos);
}

// ---- in-process JIT runs ----

TEST_CASE("codegen(jit): arithmetic and control flow") {
    CHECK_EQ(run("int s = 0; for i in [1..100]: s += i; println(s);"), std::string("5050\n"));
    CHECK_EQ(run("int n = 27, steps = 0; while n != 1: { if n % 2 == 0: n /= 2; else: n = 3 * n + 1; steps++; } "
                 "println(steps);"),
             std::string("111\n"));
}

TEST_CASE("codegen(jit): functions, recursion and dp") {
    CHECK_EQ(run("dp fn c(int n, int k) -> int { if k == 0 || k == n: return 1; return c(n-1,k-1) + c(n-1,k); } "
                 "println(c(60, 30));"),
             std::string("118264581564861424\n"));
}

TEST_CASE("codegen(jit): TC with input") {
    CHECK_EQ(run("TC: { read int a, b; println(a + b); }", "3\n1 2\n10 20\n-5 5\n"), std::string("3\n30\n0\n"));
}

TEST_CASE("codegen(jit): value semantics") {
    CHECK_EQ(run("vec<int> a = [1, 2]; vec<int> b = a; b.push(3); println(a.len(), b.len());"), std::string("23\n"));
}
