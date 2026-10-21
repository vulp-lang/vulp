#ifndef VULP_ERROR_CODE_HPP
#define VULP_ERROR_CODE_HPP

// see docs/error_codes.md

enum class ErrorCode {
    OK = 0,
    INCORRECT_USAGE = 64,
    DATA_ERROR = 65,      // lexer / parser / semantic errors
    INPUT_ERROR = 66,     // input file missing or unreadable
    INTERNAL_ERROR = 70,  // compiler bug (e.g. invalid LLVM IR)
    IO_ERROR = 74         // output / linking failure
};

constexpr int get_error_code(const ErrorCode code) {
    return static_cast<int>(code);
}

#endif // VULP_ERROR_CODE_HPP
