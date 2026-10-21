cmake_minimum_required(VERSION 3.20)
project(vulp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Keep every artifact (vulpc, libvulprt.a, tests) in the top of the build dir so the
# compiler can find its runtime library next to itself.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Vulp is pinned to the system LLVM 18 (Ubuntu llvm-18-dev). Override with -DVULP_LLVM_DIR=...
set(VULP_LLVM_DIR "/usr/lib/llvm-18/lib/cmake/llvm" CACHE PATH "Directory containing LLVMConfig.cmake")
find_package(LLVM REQUIRED CONFIG PATHS ${VULP_LLVM_DIR} NO_DEFAULT_PATH)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

set(VULP_WARNINGS -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# Runtime library: plain C++ (no LLVM). Linked into vulpc (for the JIT) and
# into every executable produced by `vulpc -o`.
# ---------------------------------------------------------------------------
add_library(vulprt_obj OBJECT runtime/vulp_runtime.cpp)
target_include_directories(vulprt_obj PUBLIC runtime)
target_compile_options(vulprt_obj PRIVATE ${VULP_WARNINGS} -O2)
set_target_properties(vulprt_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(vulprt STATIC $<TARGET_OBJECTS:vulprt_obj>)

# ---------------------------------------------------------------------------
# Front end: diagnostics, lexer, parser, AST printer, semantic analysis.
# ---------------------------------------------------------------------------
add_library(vulp_frontend STATIC
        src/misc/Diagnostics.cpp
        src/lexer/Lexer.cpp
        src/ast/AstPrinter.cpp
        src/parser/Parser.cpp
        src/sema/Types.cpp
        src/sema/Semantics.cpp
)
target_include_directories(vulp_frontend PUBLIC include)
target_include_directories(vulp_frontend SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(vulp_frontend PUBLIC ${LLVM_DEFINITIONS_LIST})
target_compile_options(vulp_frontend PRIVATE ${VULP_WARNINGS})
target_link_libraries(vulp_frontend PUBLIC LLVM)

# ---------------------------------------------------------------------------
# Back end: LLVM code generation, optimisation, JIT and object emission.
# ---------------------------------------------------------------------------
add_library(vulp_backend STATIC
        src/codegen/CodeGen.cpp
        src/codegen/CodeGenExpr.cpp
        src/codegen/CodeGenStmt.cpp
        src/codegen/CodeGenHelpers.cpp
        src/driver/Driver.cpp
)
target_link_libraries(vulp_backend PUBLIC vulp_frontend)
target_include_directories(vulp_backend PUBLIC runtime)
target_compile_options(vulp_backend PRIVATE ${VULP_WARNINGS})
target_compile_definitions(vulp_backend PRIVATE VULP_RUNTIME_LIB_DEFAULT="$<TARGET_FILE:vulprt>")
add_dependencies(vulp_backend vulprt)

add_executable(vulpc src/main.cpp $<TARGET_OBJECTS:vulprt_obj>)
target_link_libraries(vulpc PRIVATE vulp_backend pthread)
target_compile_options(vulpc PRIVATE ${VULP_WARNINGS})
# Export the runtime symbols from vulpc so JIT-compiled code can resolve them.
set_target_properties(vulpc PROPERTIES ENABLE_EXPORTS ON)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(vulp_unit_tests
        tests/unit/main.cpp
        tests/unit/LexerTests.cpp
        tests/unit/ParserTests.cpp
        tests/unit/TypesTests.cpp
        tests/unit/SemaTests.cpp
        tests/unit/RuntimeTests.cpp
        tests/unit/CodeGenTests.cpp
        $<TARGET_OBJECTS:vulprt_obj>
)
target_include_directories(vulp_unit_tests PRIVATE tests/unit runtime)
target_link_libraries(vulp_unit_tests PRIVATE vulp_backend pthread)
target_compile_options(vulp_unit_tests PRIVATE ${VULP_WARNINGS})
set_target_properties(vulp_unit_tests PROPERTIES ENABLE_EXPORTS ON)

find_package(Python3 COMPONENTS Interpreter)

add_test(NAME unit COMMAND vulp_unit_tests)
if(Python3_Interpreter_FOUND)
    add_test(NAME e2e-jit
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_tests.py
            --vulpc $<TARGET_FILE:vulpc> --mode jit)
    add_test(NAME e2e-aot
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_tests.py
            --vulpc $<TARGET_FILE:vulpc> --mode aot --workdir ${CMAKE_BINARY_DIR}/e2e-aot)
    # The same programs with VULP_DEBUG_FREE=1: freed memory is poisoned instead of being
    # released, so any use-after-free in generated code shows up as wrong output.
    add_test(NAME e2e-jit-poisoned-free
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_tests.py
            --vulpc $<TARGET_FILE:vulpc> --mode jit)
    add_test(NAME examples-poisoned-free
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_tests.py
            --vulpc $<TARGET_FILE:vulpc> --mode examples --workdir ${CMAKE_BINARY_DIR}/examples-poisoned)
    set_tests_properties(e2e-jit-poisoned-free examples-poisoned-free PROPERTIES ENVIRONMENT VULP_DEBUG_FREE=1)
    add_test(NAME compile-errors
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_tests.py
            --vulpc $<TARGET_FILE:vulpc> --mode errors)
    add_test(NAME examples
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_tests.py
            --vulpc $<TARGET_FILE:vulpc> --mode examples --workdir ${CMAKE_BINARY_DIR}/examples-aot)
endif()
