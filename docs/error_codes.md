# Exit codes

`vulpc` follows the UNIX `sysexits` convention (like the vex compiler it is modelled on).

| Code | Meaning |
|---|---|
| 0 | success (for a JIT run: the program finished normally) |
| 64 | incorrect usage (unknown option, no input file, ...) |
| 65 | the program has errors (lexer, parser or semantic errors) |
| 66 | the input file does not exist or cannot be read |
| 70 | internal compiler error (a bug: please keep the program that triggered it) |
| 74 | output / linking error (cannot write a file, runtime library or linker not found) |

When `vulpc` runs a program (`vulpc prog.vp`), its exit code is the **program's**:

| Code | Meaning |
|---|---|
| 0 | the program finished normally |
| n | the program called `exit(n)` |
| 1 | runtime error: index out of bounds, division by zero, `pop()`/`back()` on an empty container, reading past the end of input, an input integer that does not fit its type |

Executables built with `vulpc prog.vp -o prog` use the same program exit codes.
