#ifndef VULP_RUNTIME_H
#define VULP_RUNTIME_H

// The Vulp runtime: buffered competitive-programming I/O, growable sequences, ordered maps,
// memo tables for `dp fn`, and the program entry point. Compiled Vulp code calls these
// functions through the plain C ABI below. The library is linked into `vulpc` (for JIT runs)
// and into every executable built with `vulpc -o`.
//
// Memory policy: every heap block has exactly one owner (value semantics); compiled code frees
// a variable's old value when it is overwritten or goes away with its function.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Runtime layout of string, vec<T> and T[] values (24 bytes, matches the LLVM struct
// {ptr, i64, i64}). An all-zero VulpVec is a valid empty sequence.
typedef struct VulpVec {
    char* data;
    int64_t len;
    int64_t cap;
} VulpVec;

/*** memory ***/
// Releases a heap block (VULP_DEBUG_FREE=1: poisons it instead, to expose use-after-free bugs).
void vulp_rt_free(void* p);

/*** lifecycle & errors ***/
int32_t vulp_rt_start(int32_t (*entry)(void)); // runs entry on a large stack, flushes output, returns exit code
void vulp_rt_flush(void);
void vulp_rt_exit(int32_t code) __attribute__((noreturn));
void vulp_rt_panic(const char* msg, int32_t line) __attribute__((noreturn));
void vulp_rt_bounds_fail(int64_t index, int64_t len, int32_t line) __attribute__((noreturn));
void vulp_rt_div_zero(int32_t line) __attribute__((noreturn));

/*** output (buffered; flushed at exit and before blocking reads) ***/
void vulp_print_i64(int64_t v);
void vulp_print_u64(uint64_t v);
void vulp_print_i128(const void* v); // pointer to a little-endian 128-bit integer
void vulp_print_u128(const void* v);
void vulp_print_i256(const void* v); // pointer to a little-endian 256-bit integer
void vulp_print_u256(const void* v);
void vulp_print_f64(double v);       // shortest representation that round-trips
void vulp_print_char(int32_t c);
void vulp_print_bool(int32_t b);
void vulp_print_bytes(const char* p, int64_t len);
void vulp_print_str(const VulpVec* s);

/*** input (whitespace-separated tokens) ***/
// Integers that do not fit the target type are a runtime error.
int64_t vulp_read_int(int32_t bits, int32_t is_signed, int32_t line); // bits <= 64, returns the bit pattern
int64_t vulp_read_i64(int32_t line);
uint64_t vulp_read_u64(int32_t line);
void vulp_read_i128(void* out, int32_t line);
void vulp_read_u128(void* out, int32_t line);
void vulp_read_i256(void* out, int32_t line);
void vulp_read_u256(void* out, int32_t line);
double vulp_read_f64(int32_t line);
int32_t vulp_read_char(int32_t line); // next non-whitespace byte
void vulp_read_str(VulpVec* out, int32_t line);

/*** sequences (string / vec / arr) ***/
void vulp_vec_alloc(VulpVec* v, int64_t n, int64_t elem_size, int32_t line); // n zeroed elements
void* vulp_vec_push_slot(VulpVec* v, int64_t elem_size);                      // appends a zeroed slot
void vulp_vec_clone(VulpVec* dst, const VulpVec* src, int64_t elem_size);     // bytewise element copy
void vulp_str_from_bytes(VulpVec* dst, const char* p, int64_t len);
void vulp_str_concat(VulpVec* dst, const VulpVec* a, const VulpVec* b);
void vulp_str_append(VulpVec* dst, const VulpVec* src);
int32_t vulp_str_cmp(const VulpVec* a, const VulpVec* b); // <0, 0, >0
void vulp_str_from_i64(VulpVec* dst, int64_t v);
void vulp_str_from_u64(VulpVec* dst, uint64_t v);
void vulp_str_from_i128(VulpVec* dst, const void* v);
void vulp_str_from_u128(VulpVec* dst, const void* v);
void vulp_str_from_i256(VulpVec* dst, const void* v);
void vulp_str_from_u256(VulpVec* dst, const void* v);
void vulp_str_from_f64(VulpVec* dst, double v);

enum VulpSortKind {
    VULP_SORT_I8, VULP_SORT_I16, VULP_SORT_I32, VULP_SORT_I64, VULP_SORT_I128,
    VULP_SORT_U8, VULP_SORT_U16, VULP_SORT_U32, VULP_SORT_U64, VULP_SORT_U128,
    VULP_SORT_F64, VULP_SORT_STR,
};
void vulp_sort(VulpVec* v, int32_t kind);

/*** ordered maps ***/
// A map value is a pointer (NULL = empty map). Values are stored in zero-initialised
// blocks of `val_size` bytes; integer keys are passed widened to 64 bits.
enum VulpMapKeyKind { VULP_KEY_SIGNED = 0, VULP_KEY_UNSIGNED = 1, VULP_KEY_STRING = 2 };
void* vulp_map_index_i(void** slot, int32_t kind, int64_t val_size, int64_t key); // inserts a default
void* vulp_map_index_s(void** slot, int64_t val_size, const VulpVec* key);
void* vulp_map_get_i(void* m, int64_t key);                // NULL when the key is missing (no insert)
void* vulp_map_get_s(void* m, const VulpVec* key);
int32_t vulp_map_has_i(void* m, int64_t key);
int32_t vulp_map_has_s(void* m, const VulpVec* key);
void vulp_map_erase_i(void* m, int64_t key);
void vulp_map_erase_s(void* m, const VulpVec* key);
int64_t vulp_map_len(void* m);
void vulp_map_clear(void* m);
void vulp_map_free(void* m);
void* vulp_map_clone(void* m);           // value blocks are copied bytewise
void vulp_map_keys(void* m, VulpVec* out); // sorted keys: int64 elements, or VulpVec copies for strings
void vulp_map_values(void* m, VulpVec* out); // pointers to the value blocks, in key order

/*** memo tables for dp functions ***/
// Fixed-size byte keys and values. vulp_memo_get returns a pointer to the cached value or NULL;
// the pointer is only valid until the next vulp_memo_put. Clearing is O(1).
void* vulp_memo_get(void** slot, int64_t key_size, int64_t val_size, const void* key);
void vulp_memo_put(void** slot, int64_t key_size, int64_t val_size, const void* key, const void* val);
void vulp_memo_clear(void* memo);

#ifdef __cplusplus
}

#include <string>

// Test hooks (C++ only): feed stdin from a string and capture stdout into a string.
void vulp_rt_test_set_input(const std::string& data);
void vulp_rt_test_capture_begin();
std::string vulp_rt_test_capture_end();
#endif

#endif // VULP_RUNTIME_H
