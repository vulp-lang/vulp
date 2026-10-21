#ifndef VULP_DRIVER_HPP
#define VULP_DRIVER_HPP

#include <string>

struct DriverOptions {
    enum class Action {
        Run,      // JIT-compile and run (default)
        Build,    // native executable (-o)
        EmitObj,  // object file (-c)
        EmitLLVM, // LLVM IR (--emit-llvm)
        Tokens,   // --tokens
        Ast,      // --ast
        TypedAst, // --typed-ast
        Check,    // --check: front end only
    };

    std::string input;
    std::string output;   // -o
    Action action = Action::Run;
    int opt_level = 2;    // -O0 .. -O3
    bool runtime_checks = true;
    bool native_cpu = false;
    bool verbose = false;
};

// Runs the whole pipeline and returns the process exit code (see docs/error_codes.md). For
// Action::Run this is the exit code of the Vulp program itself.
int run_driver(const DriverOptions& opts);

#endif // VULP_DRIVER_HPP
