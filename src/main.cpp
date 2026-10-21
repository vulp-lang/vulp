#include <iostream>
#include <pthread.h>
#include <string>

#include "driver/Driver.hpp"
#include "misc/ErrorCode.hpp"

namespace {
    const char* USAGE = R"(Usage: vulpc [options] <file.vp>

Actions (default: --run)
  -r, --run         compile in memory with the LLVM JIT and run the program
  -o <file>         compile to a native executable
  -c                compile to an object file (name it with -o)
  --emit-llvm       print the (optimised) LLVM IR; with -o, write it to a file
  --tokens          print the token stream
  --ast             print the parsed syntax tree
  --typed-ast       print the syntax tree annotated with types
  --check           only check the program for errors

Options
  -O0 -O1 -O2 -O3   optimisation level (default -O2)
  --no-checks       disable array bounds and division-by-zero checks
  --native          tune executables for this machine's CPU
  -v, --verbose     report every compiler stage on stderr
  -h, --help        show this help
)";

    struct DriverRun {
        const DriverOptions* opts;
        int result;
    };

    void* driver_thread(void* p) {
        auto* run = static_cast<DriverRun*>(p);
        run->result = run_driver(*run->opts);
        return nullptr;
    }

    // The parser, semantic analysis and code generator are recursive over the syntax tree, so
    // very deeply nested expressions need a large stack: run the compiler on a 1 GiB one.
    int run_on_large_stack(const DriverOptions& opts) {
        DriverRun run{&opts, 0};
        pthread_attr_t attr;
        pthread_t thread;
        bool started = false;
        if (pthread_attr_init(&attr) == 0) {
            if (pthread_attr_setstacksize(&attr, static_cast<size_t>(1) << 30) == 0
                && pthread_create(&thread, &attr, driver_thread, &run) == 0) {
                started = true;
            }
            pthread_attr_destroy(&attr);
        }
        if (!started) return run_driver(opts);
        pthread_join(thread, nullptr);
        return run.result;
    }

    int usage_error(const std::string& msg) {
        std::cerr << "vulpc: " << msg << "\n\n" << USAGE;
        return get_error_code(ErrorCode::INCORRECT_USAGE);
    }
} // namespace

int main(int argc, char** argv) {
    DriverOptions opts;
    bool explicit_action = false, object_only = false;
    const auto set_action = [&](const DriverOptions::Action a) {
        opts.action = a;
        explicit_action = true;
    };

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::cout << USAGE;
            return 0;
        }
        if (arg == "-r" || arg == "--run") set_action(DriverOptions::Action::Run);
        else if (arg == "--emit-llvm") set_action(DriverOptions::Action::EmitLLVM);
        else if (arg == "--tokens") set_action(DriverOptions::Action::Tokens);
        else if (arg == "--ast") set_action(DriverOptions::Action::Ast);
        else if (arg == "--typed-ast") set_action(DriverOptions::Action::TypedAst);
        else if (arg == "--check") set_action(DriverOptions::Action::Check);
        else if (arg == "-c") object_only = true;
        else if (arg == "-o") {
            if (i + 1 >= argc) return usage_error("-o needs a file name");
            opts.output = argv[++i];
        } else if (arg == "-O0" || arg == "-O1" || arg == "-O2" || arg == "-O3") {
            opts.opt_level = arg[2] - '0';
        } else if (arg == "--no-checks") opts.runtime_checks = false;
        else if (arg == "--native") opts.native_cpu = true;
        else if (arg == "-v" || arg == "--verbose") opts.verbose = true;
        else if (!arg.empty() && arg[0] == '-') return usage_error("unknown option '" + arg + "'");
        else {
            if (!opts.input.empty()) return usage_error("more than one input file ('" + opts.input + "' and '" + arg + "')");
            opts.input = arg;
        }
    }
    if (opts.input.empty()) return usage_error("no input file");

    if (object_only) {
        if (explicit_action && opts.action != DriverOptions::Action::EmitObj) {
            return usage_error("-c cannot be combined with another action");
        }
        opts.action = DriverOptions::Action::EmitObj;
    } else if (!explicit_action && !opts.output.empty()) {
        opts.action = DriverOptions::Action::Build;
    } else if (opts.action == DriverOptions::Action::Run && !opts.output.empty()) {
        return usage_error("--run does not produce an output file; drop -o or --run");
    }
    return run_on_large_stack(opts);
}
