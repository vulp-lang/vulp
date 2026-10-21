#include "driver/Driver.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/TargetParser/Host.h>

#include "codegen/CodeGen.hpp"
#include "lexer/Lexer.hpp"
#include "misc/ErrorCode.hpp"
#include "parser/Parser.hpp"
#include "sema/Semantics.hpp"
#include "vulp_runtime.h"

namespace {
    int fail(const ErrorCode code, const std::string& msg) {
        std::cerr << "vulpc: " << msg << "\n";
        return get_error_code(code);
    }

    llvm::CodeGenOptLevel codegen_level(const int level) {
        switch (level) {
            case 0: return llvm::CodeGenOptLevel::None;
            case 1: return llvm::CodeGenOptLevel::Less;
            case 3: return llvm::CodeGenOptLevel::Aggressive;
            default: return llvm::CodeGenOptLevel::Default;
        }
    }

    void optimize(llvm::Module& module, llvm::TargetMachine* tm, const int level) {
        if (level <= 0) return;
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;
        llvm::PassBuilder pb(tm);
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        const llvm::OptimizationLevel ol = level == 1 ? llvm::OptimizationLevel::O1
                                           : level == 3 ? llvm::OptimizationLevel::O3
                                                        : llvm::OptimizationLevel::O2;
        llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(ol);
        mpm.run(module, mam);
    }

    // libvulprt.a lookup: $VULP_RUNTIME_LIB, next to the vulpc binary, or the build-time path.
    std::string find_runtime_library() {
        if (const char* env = std::getenv("VULP_RUNTIME_LIB")) return env;
        const std::string exe = llvm::sys::fs::getMainExecutable(nullptr, reinterpret_cast<void*>(&run_driver));
        if (!exe.empty()) {
            llvm::SmallString<256> candidate(llvm::sys::path::parent_path(exe));
            llvm::sys::path::append(candidate, "libvulprt.a");
            if (llvm::sys::fs::exists(candidate)) return std::string(candidate);
            candidate = llvm::sys::path::parent_path(llvm::sys::path::parent_path(exe));
            llvm::sys::path::append(candidate, "lib", "libvulprt.a");
            if (llvm::sys::fs::exists(candidate)) return std::string(candidate);
        }
#ifdef VULP_RUNTIME_LIB_DEFAULT
        return VULP_RUNTIME_LIB_DEFAULT;
#else
        return "libvulprt.a";
#endif
    }

    std::string default_output(const std::string& input, const std::string& ext) {
        llvm::SmallString<256> out(llvm::sys::path::filename(input));
        llvm::sys::path::replace_extension(out, ext);
        return std::string(out);
    }

    bool emit_object(llvm::Module& module, llvm::TargetMachine* tm, const std::string& path, std::string& err) {
        std::error_code ec;
        llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
        if (ec) {
            err = "cannot open " + path + ": " + ec.message();
            return false;
        }
        llvm::legacy::PassManager pm;
        if (tm->addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
            err = "the target cannot emit object files";
            return false;
        }
        pm.run(module);
        out.flush();
        return true;
    }

    int link_executable(const std::string& object, const std::string& output, const bool verbose) {
        const std::string runtime = find_runtime_library();
        if (!llvm::sys::fs::exists(runtime)) {
            return fail(ErrorCode::IO_ERROR, "runtime library not found: " + runtime + " (set VULP_RUNTIME_LIB)");
        }
        std::string linker_name = "c++";
        if (const char* env = std::getenv("VULP_LINKER")) linker_name = env;
        auto linker = llvm::sys::findProgramByName(linker_name);
        if (!linker) linker = llvm::sys::findProgramByName("g++");
        if (!linker) linker = llvm::sys::findProgramByName("clang++");
        if (!linker) return fail(ErrorCode::IO_ERROR, "no C++ linker driver found (set VULP_LINKER)");

        const std::vector<std::string> args_storage = {*linker, object, runtime, "-o", output, "-lpthread", "-lm"};
        std::vector<llvm::StringRef> args(args_storage.begin(), args_storage.end());
        if (verbose) {
            std::cerr << "[vulpc] link:";
            for (const auto& a : args_storage) std::cerr << " " << a;
            std::cerr << "\n";
        }
        std::string err;
        const int rc = llvm::sys::ExecuteAndWait(*linker, args, std::nullopt, {}, 0, 0, &err);
        if (rc != 0) return fail(ErrorCode::IO_ERROR, "linking failed" + (err.empty() ? "" : ": " + err));
        return 0;
    }

    void log(const DriverOptions& opts, const std::string& msg) {
        if (opts.verbose) std::cerr << "[vulpc] " << msg << "\n";
    }
} // namespace

int run_driver(const DriverOptions& opts) {
    std::ifstream file(opts.input);
    if (!file.good()) return fail(ErrorCode::INPUT_ERROR, "cannot read input file '" + opts.input + "'");
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string source = ss.str();
    Diagnostics diag(opts.input, source);

    const auto report_errors = [&](const char* stage) {
        std::cerr << diag.format();
        std::cerr << "vulpc: " << diag.count() << " error(s); compilation stopped after the " << stage << ".\n";
        return get_error_code(ErrorCode::DATA_ERROR);
    };

    // 1. lexing
    auto tokens = Lexer::tokenize(source, diag);
    if (diag.has_errors()) return report_errors("lexer");
    log(opts, "lexed " + std::to_string(tokens.size()) + " tokens");
    if (opts.action == DriverOptions::Action::Tokens) {
        for (const auto& t : tokens) std::cout << t.to_string() << "\n";
        return 0;
    }

    // 2. parsing
    Parser parser(std::move(tokens), diag);
    parser.parse();
    AST::Program program = parser.take_program();
    if (diag.has_errors()) return report_errors("parser");
    log(opts, "parsed " + std::to_string(program.items.size()) + " top-level items");
    if (opts.action == DriverOptions::Action::Ast) {
        std::cout << AST::print_program(program);
        return 0;
    }

    // 3. semantic analysis
    Sema::TypeContext types;
    SemanticAnalyzer sema(program, diag, types);
    sema.analyze();
    if (diag.has_errors()) return report_errors("semantic analysis");
    log(opts, "semantic analysis passed");
    if (opts.verbose) {
        for (const auto& fn : sema.program_info().fns) {
            if (!fn->is_dp) continue;
            log(opts, "dp fn " + fn->name + ": memoised; cache "
                          + (fn->reset_cache_per_testcase ? "cleared before every TC iteration (reads globals)"
                                                          : "kept for the whole run (pure)"));
        }
    }
    if (opts.action == DriverOptions::Action::TypedAst) {
        std::cout << AST::print_program_typed(program);
        return 0;
    }
    if (opts.action == DriverOptions::Action::Check) return 0;

    // 4. LLVM code generation
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    const bool jit = opts.action == DriverOptions::Action::Run;
    std::unique_ptr<llvm::TargetMachine> tm;
    std::optional<llvm::orc::JITTargetMachineBuilder> jtmb;
    if (jit) {
        auto detected = llvm::orc::JITTargetMachineBuilder::detectHost();
        if (!detected) return fail(ErrorCode::INTERNAL_ERROR, llvm::toString(detected.takeError()));
        detected->setCodeGenOptLevel(codegen_level(opts.opt_level));
        jtmb = std::move(*detected);
        auto created = jtmb->createTargetMachine();
        if (!created) return fail(ErrorCode::INTERNAL_ERROR, llvm::toString(created.takeError()));
        tm = std::move(*created);
    } else {
        const std::string triple = llvm::sys::getDefaultTargetTriple();
        std::string err;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
        if (!target) return fail(ErrorCode::INTERNAL_ERROR, "no target for " + triple + ": " + err);
        std::string cpu = "generic", features;
        if (opts.native_cpu) {
            cpu = llvm::sys::getHostCPUName().str();
            llvm::SubtargetFeatures f;
            llvm::StringMap<bool> host_features;
            if (llvm::sys::getHostCPUFeatures(host_features)) {
                for (const auto& feature : host_features) f.AddFeature(feature.first(), feature.second);
            }
            features = f.getString();
        }
        tm.reset(target->createTargetMachine(triple, cpu, features, llvm::TargetOptions(), llvm::Reloc::PIC_,
                                             std::nullopt, codegen_level(opts.opt_level)));
        if (!tm) return fail(ErrorCode::INTERNAL_ERROR, "cannot create a target machine for " + triple);
    }

    auto context = std::make_unique<llvm::LLVMContext>();
    CodeGenOptions cg_opts;
    cg_opts.runtime_checks = opts.runtime_checks;
    cg_opts.emit_c_main = opts.action == DriverOptions::Action::Build || opts.action == DriverOptions::Action::EmitObj;
    std::unique_ptr<llvm::Module> module;
    try {
        CodeGen codegen(program, sema.program_info(), types, *context, tm->createDataLayout(),
                        tm->getTargetTriple().str(), cg_opts);
        module = codegen.generate();
    } catch (const CodeGenError& e) {
        return fail(ErrorCode::INTERNAL_ERROR, std::string("internal code generation error: ") + e.what());
    }
    log(opts, "generated LLVM IR");

    // 5. optimisation
    optimize(*module, tm.get(), opts.opt_level);
    log(opts, "optimised with -O" + std::to_string(opts.opt_level));

    // 6. output
    switch (opts.action) {
        case DriverOptions::Action::EmitLLVM: {
            if (opts.output.empty() || opts.output == "-") {
                module->print(llvm::outs(), nullptr);
                return 0;
            }
            std::error_code ec;
            llvm::raw_fd_ostream out(opts.output, ec, llvm::sys::fs::OF_Text);
            if (ec) return fail(ErrorCode::IO_ERROR, "cannot write " + opts.output + ": " + ec.message());
            module->print(out, nullptr);
            return 0;
        }
        case DriverOptions::Action::EmitObj: {
            const std::string out = opts.output.empty() ? default_output(opts.input, "o") : opts.output;
            std::string err;
            if (!emit_object(*module, tm.get(), out, err)) return fail(ErrorCode::IO_ERROR, err);
            log(opts, "wrote " + out);
            return 0;
        }
        case DriverOptions::Action::Build: {
            const std::string out = opts.output.empty() ? default_output(opts.input, "") : opts.output;
            llvm::SmallString<256> obj;
            if (llvm::sys::fs::createTemporaryFile("vulp", "o", obj)) {
                return fail(ErrorCode::IO_ERROR, "cannot create a temporary object file");
            }
            std::string err;
            if (!emit_object(*module, tm.get(), std::string(obj), err)) return fail(ErrorCode::IO_ERROR, err);
            const int rc = link_executable(std::string(obj), out, opts.verbose);
            llvm::sys::fs::remove(obj);
            if (rc == 0) log(opts, "wrote executable " + out);
            return rc;
        }
        case DriverOptions::Action::Run: {
            auto jit_or_err = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
            if (!jit_or_err) return fail(ErrorCode::INTERNAL_ERROR, llvm::toString(jit_or_err.takeError()));
            auto& lljit = *jit_or_err;
            // Runtime functions are exported from vulpc itself.
            auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
                lljit->getDataLayout().getGlobalPrefix());
            if (!gen) return fail(ErrorCode::INTERNAL_ERROR, llvm::toString(gen.takeError()));
            lljit->getMainJITDylib().addGenerator(std::move(*gen));
            if (auto e = lljit->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context)))) {
                return fail(ErrorCode::INTERNAL_ERROR, llvm::toString(std::move(e)));
            }
            auto sym = lljit->lookup("vulp_main");
            if (!sym) return fail(ErrorCode::INTERNAL_ERROR, llvm::toString(sym.takeError()));
            auto* entry = sym->toPtr<int32_t (*)()>();
            log(opts, "running (JIT)");
            std::cout.flush();
            std::fflush(stdout);
            return vulp_rt_start(entry);
        }
        default:
            return 0;
    }
}
