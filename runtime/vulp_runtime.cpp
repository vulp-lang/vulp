#include "vulp_runtime.h"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <map>
#include <pthread.h>
#include <string>
#include <unistd.h>

__extension__ typedef __int128 vulp_i128;
__extension__ typedef unsigned __int128 vulp_u128;

namespace {
    /*** output buffer ***/

    constexpr size_t OUT_CAP = 1 << 16;
    char out_buf[OUT_CAP];
    size_t out_len = 0;
    bool capturing = false;
    std::string captured;

    void write_all(const char* p, size_t n) {
        if (capturing) {
            captured.append(p, n);
            return;
        }
        while (n > 0) {
            const ssize_t w = ::write(1, p, n);
            if (w < 0) {
                if (errno == EINTR) continue;
                return; // stdout closed: drop output
            }
            p += w;
            n -= static_cast<size_t>(w);
        }
    }

    void out_flush() {
        if (out_len) write_all(out_buf, out_len);
        out_len = 0;
    }

    void out_bytes(const char* p, size_t n) {
        if (n >= OUT_CAP) {
            out_flush();
            write_all(p, n);
            return;
        }
        if (out_len + n > OUT_CAP) out_flush();
        std::memcpy(out_buf + out_len, p, n);
        out_len += n;
    }

    void out_char(const char c) {
        if (out_len == OUT_CAP) out_flush();
        out_buf[out_len++] = c;
    }

    /*** input buffer ***/

    constexpr size_t IN_CAP = 1 << 16;
    char in_buf[IN_CAP];
    size_t in_pos = 0, in_len = 0;
    bool in_eof = false;
    bool test_input = false;
    std::string test_input_data;
    size_t test_input_pos = 0;

    bool refill() {
        if (in_eof) return false;
        out_flush(); // interactive problems: show our output before blocking on input
        if (test_input) {
            const size_t n = std::min(IN_CAP, test_input_data.size() - test_input_pos);
            std::memcpy(in_buf, test_input_data.data() + test_input_pos, n);
            test_input_pos += n;
            in_pos = 0;
            in_len = n;
            if (n == 0) in_eof = true;
            return n > 0;
        }
        while (true) {
            const ssize_t r = ::read(0, in_buf, IN_CAP);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) {
                in_eof = true;
                in_pos = in_len = 0;
                return false;
            }
            in_pos = 0;
            in_len = static_cast<size_t>(r);
            return true;
        }
    }

    // -1 at end of input
    int peek_byte() {
        if (in_pos == in_len && !refill()) return -1;
        return static_cast<unsigned char>(in_buf[in_pos]);
    }

    int next_byte() {
        const int c = peek_byte();
        if (c >= 0) in_pos++;
        return c;
    }

    bool is_space(const int c) {
        return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
    }

    // Skips whitespace; panics at end of input.
    int skip_space(const char* what, const int32_t line) {
        int c;
        while ((c = peek_byte()) >= 0 && is_space(c)) in_pos++;
        if (c < 0) {
            std::string msg = std::string("unexpected end of input while reading ") + what;
            vulp_rt_panic(msg.c_str(), line);
        }
        return c;
    }

    std::string read_token(const char* what, const int32_t line) {
        skip_space(what, line);
        std::string tok;
        int c;
        while ((c = peek_byte()) >= 0 && !is_space(c)) {
            tok += static_cast<char>(c);
            in_pos++;
        }
        return tok;
    }

    /*** 256-bit helpers (4 little-endian 64-bit limbs) ***/

    struct U256 {
        uint64_t w[4] = {0, 0, 0, 0};
    };

    bool u256_is_zero(const U256& x) {
        return (x.w[0] | x.w[1] | x.w[2] | x.w[3]) == 0;
    }

    void u256_negate(U256& x) {
        vulp_u128 carry = 1;
        for (auto& limb : x.w) {
            const vulp_u128 s = static_cast<vulp_u128>(~limb) + carry;
            limb = static_cast<uint64_t>(s);
            carry = s >> 64;
        }
    }

    // x = x * mul + add; returns true when the result no longer fits in 256 bits.
    bool u256_mul_add(U256& x, const uint64_t mul, const uint64_t add) {
        vulp_u128 carry = add;
        for (auto& limb : x.w) {
            const vulp_u128 p = static_cast<vulp_u128>(limb) * mul + carry;
            limb = static_cast<uint64_t>(p);
            carry = p >> 64;
        }
        return carry != 0;
    }

    int u256_bit_length(const U256& x) {
        for (int i = 3; i >= 0; i--) {
            if (x.w[i]) return i * 64 + 64 - __builtin_clzll(x.w[i]);
        }
        return 0;
    }

    int u256_popcount(const U256& x) {
        return __builtin_popcountll(x.w[0]) + __builtin_popcountll(x.w[1]) + __builtin_popcountll(x.w[2])
               + __builtin_popcountll(x.w[3]);
    }

    uint64_t u256_divmod(U256& x, const uint64_t div) {
        vulp_u128 rem = 0;
        for (int i = 3; i >= 0; i--) {
            const vulp_u128 cur = (rem << 64) | x.w[i];
            x.w[i] = static_cast<uint64_t>(cur / div);
            rem = cur % div;
        }
        return static_cast<uint64_t>(rem);
    }

    // Decimal formatting into `buf` (at least 100 bytes); each returns the number of characters.
    int format_u256(char* buf, U256 x, const bool negative) {
        int n = 0;
        if (u256_is_zero(x)) buf[n++] = '0';
        while (!u256_is_zero(x)) buf[n++] = static_cast<char>('0' + u256_divmod(x, 10));
        if (negative) buf[n++] = '-';
        std::reverse(buf, buf + n);
        return n;
    }

    int format_u128(char* buf, vulp_u128 x, const bool negative) {
        int n = 0;
        if (x == 0) buf[n++] = '0';
        while (x) {
            buf[n++] = static_cast<char>('0' + static_cast<int>(x % 10));
            x /= 10;
        }
        if (negative) buf[n++] = '-';
        std::reverse(buf, buf + n);
        return n;
    }

    int format_i128(char* buf, const void* v) {
        vulp_i128 x;
        std::memcpy(&x, v, sizeof x);
        const bool negative = x < 0;
        return format_u128(buf, negative ? 0 - static_cast<vulp_u128>(x) : static_cast<vulp_u128>(x), negative);
    }

    int format_i256(char* buf, const void* v) {
        U256 x;
        std::memcpy(x.w, v, sizeof x.w);
        const bool negative = (x.w[3] >> 63) != 0;
        if (negative) u256_negate(x);
        return format_u256(buf, x, negative);
    }

    // The shortest decimal that reads back as exactly the same double. Like Python's repr,
    // plain notation is used for decimal exponents in [-4, 16) and scientific notation
    // (1e+20, 1e-05) otherwise; whole numbers print without a fraction ("100").
    int format_f64(char* buf, const double v) {
        if (std::isnan(v)) {
            std::memcpy(buf, "nan", 3);
            return 3;
        }
        if (std::isinf(v)) {
            std::memcpy(buf, v < 0 ? "-inf" : "inf", v < 0 ? 4 : 3);
            return v < 0 ? 4 : 3;
        }
        char sci[64];
        const auto res = std::to_chars(sci, sci + sizeof sci - 1, v, std::chars_format::scientific);
        *res.ptr = '\0'; // to_chars does not terminate; std::atoi below needs it
        const char* p = sci;
        int n = 0;
        if (*p == '-') {
            buf[n++] = '-';
            p++;
        }
        char digits[32] = {};
        int nd = 0;
        while (p < res.ptr && *p != 'e') {
            if (*p != '.') digits[nd++] = *p;
            p++;
        }
        const int exp = std::atoi(p + 1);
        if (exp >= -4 && exp < 16) {
            if (exp >= 0) {
                const int int_len = exp + 1;
                for (int i = 0; i < int_len; i++) buf[n++] = i < nd ? digits[i] : '0';
                if (nd > int_len) {
                    buf[n++] = '.';
                    for (int i = int_len; i < nd; i++) buf[n++] = digits[i];
                }
            } else {
                buf[n++] = '0';
                buf[n++] = '.';
                for (int i = 0; i < -exp - 1; i++) buf[n++] = '0';
                for (int i = 0; i < nd; i++) buf[n++] = digits[i];
            }
        } else {
            buf[n++] = digits[0];
            if (nd > 1) {
                buf[n++] = '.';
                for (int i = 1; i < nd; i++) buf[n++] = digits[i];
            }
            n += std::snprintf(buf + n, 16, "e%c%02d", exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
        }
        return n;
    }

    // Parses an optionally signed decimal token into 256 bits (wrapping on overflow).
    U256 read_u256_token(const char* what, const int32_t line, const bool allow_sign, bool& negative,
                         bool& overflow) {
        skip_space(what, line);
        negative = false;
        int c = peek_byte();
        if (c == '-' || c == '+') {
            if (c == '-') {
                if (!allow_sign) vulp_rt_panic("expected an unsigned integer but found '-'", line);
                negative = true;
            }
            in_pos++;
        }
        U256 x;
        bool any = false;
        overflow = false;
        while ((c = peek_byte()) >= '0' && c <= '9') {
            if (u256_mul_add(x, 10, static_cast<uint64_t>(c - '0'))) overflow = true;
            in_pos++;
            any = true;
        }
        if (!any) {
            const std::string msg = std::string("expected ") + what + " in the input";
            vulp_rt_panic(msg.c_str(), line);
        }
        return x;
    }

    // Reads an integer token and panics unless it fits in a `bits`-bit integer.
    U256 read_checked(const int bits, const bool is_signed, const int32_t line, bool& negative) {
        bool overflow;
        U256 mag = read_u256_token(is_signed ? "an integer" : "an unsigned integer", line, is_signed, negative,
                                   overflow);
        const int len = u256_bit_length(mag);
        bool ok = !overflow;
        if (ok && is_signed && len > bits - 1) {
            ok = negative && len == bits && u256_popcount(mag) == 1; // the minimum value -2^(bits-1)
        }
        if (ok && !is_signed && len > bits) ok = false;
        if (!ok) {
            char msg[96];
            std::snprintf(msg, sizeof msg, "integer in the input does not fit in %s%d", is_signed ? "i" : "u", bits);
            vulp_rt_panic(msg, line);
        }
        if (negative) u256_negate(mag);
        return mag; // two's complement bit pattern
    }

    /*** large stack entry ***/

    struct StartArgs {
        int32_t (*entry)();
        int32_t result;
    };

    void* start_thread(void* p) {
        auto* args = static_cast<StartArgs*>(p);
        args->result = args->entry();
        return nullptr;
    }

    // VULP_DEBUG_FREE=1: poison freed blocks instead of freeing them, so that a use after
    // free shows up as garbage output in tests.
    bool debug_free() {
        static const bool on = std::getenv("VULP_DEBUG_FREE") != nullptr;
        return on;
    }

    void release(void* p) {
        if (!p) return;
        if (debug_free()) {
            std::memset(p, 0xA5, malloc_usable_size(p));
            return;
        }
        std::free(p);
    }

    void* xmalloc(const size_t n) {
        void* p = std::malloc(n ? n : 1);
        if (!p) vulp_rt_panic("out of memory", 0);
        return p;
    }

    void* xcalloc(const size_t n) {
        void* p = std::calloc(n ? n : 1, 1);
        if (!p) vulp_rt_panic("out of memory", 0);
        return p;
    }

    void ensure_cap(VulpVec* v, const int64_t need, const int64_t elem_size) {
        if (need <= v->cap) return;
        int64_t cap = v->cap ? v->cap * 2 : 4;
        if (cap < need) cap = need;
        char* data = static_cast<char*>(std::realloc(v->data, static_cast<size_t>(cap * elem_size)));
        if (!data) vulp_rt_panic("out of memory", 0);
        std::memset(data + v->cap * elem_size, 0, static_cast<size_t>((cap - v->cap) * elem_size));
        v->data = data;
        v->cap = cap;
    }

    /*** maps ***/

    std::string key_string(const VulpVec* s) {
        return s->len ? std::string(s->data, static_cast<size_t>(s->len)) : std::string();
    }

    struct VulpMap {
        int32_t kind;
        int64_t val_size;
        std::map<int64_t, char*> signed_keys;
        std::map<uint64_t, char*> unsigned_keys;
        std::map<std::string, char*> string_keys;

        [[nodiscard]] int64_t size() const {
            switch (kind) {
                case VULP_KEY_SIGNED: return static_cast<int64_t>(signed_keys.size());
                case VULP_KEY_UNSIGNED: return static_cast<int64_t>(unsigned_keys.size());
                default: return static_cast<int64_t>(string_keys.size());
            }
        }
    };

    VulpMap* map_slot(void** slot, const int32_t kind, const int64_t val_size) {
        if (!*slot) {
            auto* m = new VulpMap();
            m->kind = kind;
            m->val_size = val_size;
            *slot = m;
        }
        return static_cast<VulpMap*>(*slot);
    }

    template<class M, class K>
    char* map_index(M& map, const K& key, const int64_t val_size) {
        auto it = map.find(key);
        if (it != map.end()) return it->second;
        char* block = static_cast<char*>(xcalloc(static_cast<size_t>(val_size)));
        map.emplace(key, block);
        return block;
    }

    template<class M>
    void map_clone_into(const M& src, M& dst, const int64_t val_size) {
        for (const auto& [k, v] : src) {
            char* block = static_cast<char*>(xmalloc(static_cast<size_t>(val_size)));
            std::memcpy(block, v, static_cast<size_t>(val_size));
            dst.emplace(k, block);
        }
    }

    /*** memo tables (open addressing, linear probing) ***/

    // Entries are tagged with the generation they were written in; clearing a table just starts
    // a new generation, so it costs O(1) however large the table has grown.
    struct Memo {
        int64_t key_size, val_size, entry_size;
        int64_t cap = 0, count = 0;
        uint32_t gen = 1;
        char* table = nullptr; // entry: [generation:4][key][value]
    };

    constexpr int64_t MEMO_TAG = 4;

    uint32_t entry_gen(const char* e) {
        uint32_t g;
        std::memcpy(&g, e, sizeof g);
        return g;
    }

    uint64_t hash_bytes(const void* p, const int64_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        uint64_t h = 1469598103934665603ULL;
        for (int64_t i = 0; i < n; i++) {
            h ^= b[i];
            h *= 1099511628211ULL;
        }
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        return h;
    }

    char* memo_find(const Memo* m, const void* key, bool& found) {
        const uint64_t mask = static_cast<uint64_t>(m->cap - 1);
        uint64_t i = hash_bytes(key, m->key_size) & mask;
        while (true) {
            char* e = m->table + static_cast<int64_t>(i) * m->entry_size;
            if (entry_gen(e) != m->gen) {
                found = false;
                return e;
            }
            if (std::memcmp(e + MEMO_TAG, key, static_cast<size_t>(m->key_size)) == 0) {
                found = true;
                return e;
            }
            i = (i + 1) & mask;
        }
    }

    void memo_grow(Memo* m) {
        const int64_t old_cap = m->cap;
        char* old = m->table;
        m->cap = old_cap ? old_cap * 2 : 1024;
        m->table = static_cast<char*>(xcalloc(static_cast<size_t>(m->cap * m->entry_size)));
        m->count = 0;
        for (int64_t i = 0; i < old_cap; i++) {
            char* e = old + i * m->entry_size;
            if (entry_gen(e) != m->gen) continue;
            bool found;
            char* slot = memo_find(m, e + MEMO_TAG, found);
            std::memcpy(slot, e, static_cast<size_t>(m->entry_size));
            m->count++;
        }
        std::free(old);
    }

    Memo* memo_slot(void** slot, const int64_t key_size, const int64_t val_size) {
        if (!*slot) {
            auto* m = new Memo();
            m->key_size = key_size;
            m->val_size = val_size;
            m->entry_size = MEMO_TAG + key_size + val_size;
            *slot = m;
        }
        return static_cast<Memo*>(*slot);
    }

    template<class T>
    void sort_as(VulpVec* v) {
        T* p = reinterpret_cast<T*>(v->data);
        std::sort(p, p + v->len);
    }
} // namespace

extern "C" {

/*** lifecycle & errors ***/

int32_t vulp_rt_start(int32_t (*entry)(void)) {
    StartArgs args{entry, 0};
    pthread_attr_t attr;
    pthread_t thread;
    bool started = false;
    if (pthread_attr_init(&attr) == 0) {
        // Deep recursion (DFS, memoised dp) is common in contest code: give it a 1 GiB stack.
        if (pthread_attr_setstacksize(&attr, static_cast<size_t>(1) << 30) == 0
            && pthread_create(&thread, &attr, start_thread, &args) == 0) {
            started = true;
        }
        pthread_attr_destroy(&attr);
    }
    if (started) {
        pthread_join(thread, nullptr);
    } else {
        args.result = entry();
    }
    out_flush();
    return args.result;
}

void vulp_rt_flush(void) {
    out_flush();
}

void vulp_rt_exit(const int32_t code) {
    out_flush();
    std::fflush(stdout);
    std::exit(code);
}

void vulp_rt_panic(const char* msg, const int32_t line) {
    out_flush();
    if (line > 0) std::fprintf(stderr, "vulp runtime error (line %d): %s\n", line, msg);
    else std::fprintf(stderr, "vulp runtime error: %s\n", msg);
    std::fflush(stderr);
    std::exit(1);
}

void vulp_rt_bounds_fail(const int64_t index, const int64_t len, const int32_t line) {
    char msg[128];
    std::snprintf(msg, sizeof msg, "index %lld out of bounds for length %lld", static_cast<long long>(index),
                  static_cast<long long>(len));
    vulp_rt_panic(msg, line);
}

void vulp_rt_div_zero(const int32_t line) {
    vulp_rt_panic("division by zero", line);
}

/*** output ***/

void vulp_print_i64(const int64_t v) {
    char buf[24];
    int n = 0;
    uint64_t x = v < 0 ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
    if (x == 0) buf[n++] = '0';
    while (x) {
        buf[n++] = static_cast<char>('0' + x % 10);
        x /= 10;
    }
    if (v < 0) buf[n++] = '-';
    std::reverse(buf, buf + n);
    out_bytes(buf, static_cast<size_t>(n));
}

void vulp_print_u64(uint64_t v) {
    char buf[24];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v) {
        buf[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    }
    std::reverse(buf, buf + n);
    out_bytes(buf, static_cast<size_t>(n));
}

void vulp_print_i128(const void* v) {
    char buf[100];
    out_bytes(buf, static_cast<size_t>(format_i128(buf, v)));
}

void vulp_print_u128(const void* v) {
    vulp_u128 x;
    std::memcpy(&x, v, sizeof x);
    char buf[100];
    out_bytes(buf, static_cast<size_t>(format_u128(buf, x, false)));
}

void vulp_print_i256(const void* v) {
    char buf[100];
    out_bytes(buf, static_cast<size_t>(format_i256(buf, v)));
}

void vulp_print_u256(const void* v) {
    U256 x;
    std::memcpy(x.w, v, sizeof x.w);
    char buf[100];
    out_bytes(buf, static_cast<size_t>(format_u256(buf, x, false)));
}

void vulp_print_f64(const double v) {
    char buf[64];
    out_bytes(buf, static_cast<size_t>(format_f64(buf, v)));
}

void vulp_print_char(const int32_t c) {
    out_char(static_cast<char>(c));
}

void vulp_print_bool(const int32_t b) {
    if (b) out_bytes("true", 4);
    else out_bytes("false", 5);
}

void vulp_print_bytes(const char* p, const int64_t len) {
    if (len > 0) out_bytes(p, static_cast<size_t>(len));
}

void vulp_print_str(const VulpVec* s) {
    if (s->len > 0) out_bytes(s->data, static_cast<size_t>(s->len));
}

/*** input ***/

int64_t vulp_read_int(const int32_t bits, const int32_t is_signed, const int32_t line) {
    bool negative;
    const U256 x = read_checked(bits, is_signed != 0, line, negative);
    return static_cast<int64_t>(x.w[0]);
}

int64_t vulp_read_i64(const int32_t line) {
    return vulp_read_int(64, 1, line);
}

uint64_t vulp_read_u64(const int32_t line) {
    return static_cast<uint64_t>(vulp_read_int(64, 0, line));
}

void vulp_read_i128(void* out, const int32_t line) {
    bool negative;
    const U256 x = read_checked(128, true, line, negative);
    std::memcpy(out, x.w, 16);
}

void vulp_read_u128(void* out, const int32_t line) {
    bool negative;
    const U256 x = read_checked(128, false, line, negative);
    std::memcpy(out, x.w, 16);
}

void vulp_read_i256(void* out, const int32_t line) {
    bool negative;
    const U256 x = read_checked(256, true, line, negative);
    std::memcpy(out, x.w, 32);
}

void vulp_read_u256(void* out, const int32_t line) {
    bool negative;
    const U256 x = read_checked(256, false, line, negative);
    std::memcpy(out, x.w, 32);
}

double vulp_read_f64(const int32_t line) {
    const std::string tok = read_token("a float", line);
    char* end = nullptr;
    const double v = std::strtod(tok.c_str(), &end);
    if (end == tok.c_str() || *end != '\0') {
        const std::string msg = "expected a float in the input, found '" + tok + "'";
        vulp_rt_panic(msg.c_str(), line);
    }
    return v;
}

int32_t vulp_read_char(const int32_t line) {
    skip_space("a char", line);
    return next_byte();
}

void vulp_read_str(VulpVec* out, const int32_t line) {
    skip_space("a string", line);
    out->len = 0; // the buffer is exclusively owned (value semantics), so it can be reused
    int c;
    while ((c = peek_byte()) >= 0 && !is_space(c)) {
        ensure_cap(out, out->len + 1, 1);
        out->data[out->len++] = static_cast<char>(c);
        in_pos++;
    }
}

/*** sequences ***/

void vulp_vec_alloc(VulpVec* v, const int64_t n, const int64_t elem_size, const int32_t line) {
    if (n < 0) {
        char msg[96];
        std::snprintf(msg, sizeof msg, "negative array size %lld", static_cast<long long>(n));
        vulp_rt_panic(msg, line);
    }
    v->data = n ? static_cast<char*>(xcalloc(static_cast<size_t>(n * elem_size))) : nullptr;
    v->len = n;
    v->cap = n;
}

void* vulp_vec_push_slot(VulpVec* v, const int64_t elem_size) {
    ensure_cap(v, v->len + 1, elem_size);
    char* slot = v->data + v->len * elem_size;
    std::memset(slot, 0, static_cast<size_t>(elem_size));
    v->len++;
    return slot;
}

void vulp_vec_clone(VulpVec* dst, const VulpVec* src, const int64_t elem_size) {
    const int64_t n = src->len;
    char* data = nullptr;
    if (n) {
        data = static_cast<char*>(xmalloc(static_cast<size_t>(n * elem_size)));
        std::memcpy(data, src->data, static_cast<size_t>(n * elem_size));
    }
    dst->data = data;
    dst->len = n;
    dst->cap = n;
}

void vulp_str_from_bytes(VulpVec* dst, const char* p, const int64_t len) {
    dst->data = nullptr;
    dst->len = dst->cap = 0;
    if (len <= 0) return;
    dst->data = static_cast<char*>(xmalloc(static_cast<size_t>(len)));
    std::memcpy(dst->data, p, static_cast<size_t>(len));
    dst->len = dst->cap = len;
}

void vulp_str_concat(VulpVec* dst, const VulpVec* a, const VulpVec* b) {
    const int64_t n = a->len + b->len;
    char* data = n ? static_cast<char*>(xmalloc(static_cast<size_t>(n))) : nullptr;
    if (a->len) std::memcpy(data, a->data, static_cast<size_t>(a->len));
    if (b->len) std::memcpy(data + a->len, b->data, static_cast<size_t>(b->len));
    dst->data = data;
    dst->len = dst->cap = n;
}

void vulp_str_append(VulpVec* dst, const VulpVec* src) {
    const int64_t add = src->len;
    if (!add) return;
    if (dst == src || dst->data == src->data) { // s += s
        const VulpVec copy = *src;
        char* tmp = static_cast<char*>(xmalloc(static_cast<size_t>(add)));
        std::memcpy(tmp, copy.data, static_cast<size_t>(add));
        ensure_cap(dst, dst->len + add, 1);
        std::memcpy(dst->data + dst->len, tmp, static_cast<size_t>(add));
        std::free(tmp);
    } else {
        ensure_cap(dst, dst->len + add, 1);
        std::memcpy(dst->data + dst->len, src->data, static_cast<size_t>(add));
    }
    dst->len += add;
}

int32_t vulp_str_cmp(const VulpVec* a, const VulpVec* b) {
    const int64_t n = std::min(a->len, b->len);
    if (n > 0) {
        const int c = std::memcmp(a->data, b->data, static_cast<size_t>(n));
        if (c != 0) return c < 0 ? -1 : 1;
    }
    if (a->len == b->len) return 0;
    return a->len < b->len ? -1 : 1;
}

void vulp_str_from_i64(VulpVec* dst, const int64_t v) {
    char buf[24];
    const int n = std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    vulp_str_from_bytes(dst, buf, n);
}

void vulp_str_from_u64(VulpVec* dst, const uint64_t v) {
    char buf[24];
    const int n = std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(v));
    vulp_str_from_bytes(dst, buf, n);
}

void vulp_str_from_i128(VulpVec* dst, const void* v) {
    char buf[100];
    vulp_str_from_bytes(dst, buf, format_i128(buf, v));
}

void vulp_str_from_u128(VulpVec* dst, const void* v) {
    vulp_u128 x;
    std::memcpy(&x, v, sizeof x);
    char buf[100];
    vulp_str_from_bytes(dst, buf, format_u128(buf, x, false));
}

void vulp_str_from_i256(VulpVec* dst, const void* v) {
    char buf[100];
    vulp_str_from_bytes(dst, buf, format_i256(buf, v));
}

void vulp_str_from_u256(VulpVec* dst, const void* v) {
    U256 x;
    std::memcpy(x.w, v, sizeof x.w);
    char buf[100];
    vulp_str_from_bytes(dst, buf, format_u256(buf, x, false));
}

void vulp_str_from_f64(VulpVec* dst, const double v) {
    char buf[64];
    vulp_str_from_bytes(dst, buf, format_f64(buf, v));
}

void vulp_sort(VulpVec* v, const int32_t kind) {
    if (v->len < 2) return;
    switch (kind) {
        case VULP_SORT_I8: sort_as<int8_t>(v); break;
        case VULP_SORT_I16: sort_as<int16_t>(v); break;
        case VULP_SORT_I32: sort_as<int32_t>(v); break;
        case VULP_SORT_I64: sort_as<int64_t>(v); break;
        case VULP_SORT_I128: sort_as<vulp_i128>(v); break;
        case VULP_SORT_U8: sort_as<uint8_t>(v); break;
        case VULP_SORT_U16: sort_as<uint16_t>(v); break;
        case VULP_SORT_U32: sort_as<uint32_t>(v); break;
        case VULP_SORT_U64: sort_as<uint64_t>(v); break;
        case VULP_SORT_U128: sort_as<vulp_u128>(v); break;
        case VULP_SORT_F64: sort_as<double>(v); break;
        case VULP_SORT_STR: {
            auto* p = reinterpret_cast<VulpVec*>(v->data);
            std::sort(p, p + v->len, [](const VulpVec& a, const VulpVec& b) { return vulp_str_cmp(&a, &b) < 0; });
            break;
        }
        default: vulp_rt_panic("internal error: unknown sort kind", 0);
    }
}

/*** maps ***/

void* vulp_map_index_i(void** slot, const int32_t kind, const int64_t val_size, const int64_t key) {
    VulpMap* m = map_slot(slot, kind, val_size);
    if (m->kind == VULP_KEY_UNSIGNED) return map_index(m->unsigned_keys, static_cast<uint64_t>(key), val_size);
    return map_index(m->signed_keys, key, val_size);
}

void* vulp_map_index_s(void** slot, const int64_t val_size, const VulpVec* key) {
    VulpMap* m = map_slot(slot, VULP_KEY_STRING, val_size);
    return map_index(m->string_keys, key_string(key), val_size);
}

int32_t vulp_map_has_i(void* p, const int64_t key) {
    if (!p) return 0;
    const auto* m = static_cast<VulpMap*>(p);
    if (m->kind == VULP_KEY_UNSIGNED) return m->unsigned_keys.contains(static_cast<uint64_t>(key));
    return m->signed_keys.contains(key);
}

int32_t vulp_map_has_s(void* p, const VulpVec* key) {
    if (!p) return 0;
    return static_cast<VulpMap*>(p)->string_keys.contains(key_string(key));
}

void* vulp_map_get_i(void* p, const int64_t key) {
    if (!p) return nullptr;
    auto* m = static_cast<VulpMap*>(p);
    if (m->kind == VULP_KEY_UNSIGNED) {
        const auto it = m->unsigned_keys.find(static_cast<uint64_t>(key));
        return it == m->unsigned_keys.end() ? nullptr : it->second;
    }
    const auto it = m->signed_keys.find(key);
    return it == m->signed_keys.end() ? nullptr : it->second;
}

void* vulp_map_get_s(void* p, const VulpVec* key) {
    if (!p) return nullptr;
    auto& keys = static_cast<VulpMap*>(p)->string_keys;
    const auto it = keys.find(key_string(key));
    return it == keys.end() ? nullptr : it->second;
}

void vulp_map_erase_i(void* p, const int64_t key) {
    if (!p) return;
    auto* m = static_cast<VulpMap*>(p);
    if (m->kind == VULP_KEY_UNSIGNED) {
        const auto it = m->unsigned_keys.find(static_cast<uint64_t>(key));
        if (it == m->unsigned_keys.end()) return;
        release(it->second);
        m->unsigned_keys.erase(it);
    } else {
        const auto it = m->signed_keys.find(key);
        if (it == m->signed_keys.end()) return;
        release(it->second);
        m->signed_keys.erase(it);
    }
}

void vulp_map_erase_s(void* p, const VulpVec* key) {
    if (!p) return;
    auto& keys = static_cast<VulpMap*>(p)->string_keys;
    const auto it = keys.find(key_string(key));
    if (it == keys.end()) return;
    release(it->second);
    keys.erase(it);
}

int64_t vulp_map_len(void* p) {
    return p ? static_cast<VulpMap*>(p)->size() : 0;
}

void vulp_map_clear(void* p) {
    if (!p) return;
    auto* m = static_cast<VulpMap*>(p);
    for (const auto& kv : m->signed_keys) release(kv.second);
    for (const auto& kv : m->unsigned_keys) release(kv.second);
    for (const auto& kv : m->string_keys) release(kv.second);
    m->signed_keys.clear();
    m->unsigned_keys.clear();
    m->string_keys.clear();
}

void vulp_map_free(void* p) {
    if (!p) return;
    vulp_map_clear(p);
    delete static_cast<VulpMap*>(p);
}

void vulp_rt_free(void* p) {
    release(p);
}

void* vulp_map_clone(void* p) {
    if (!p) return nullptr;
    const auto* src = static_cast<VulpMap*>(p);
    auto* dst = new VulpMap();
    dst->kind = src->kind;
    dst->val_size = src->val_size;
    map_clone_into(src->signed_keys, dst->signed_keys, src->val_size);
    map_clone_into(src->unsigned_keys, dst->unsigned_keys, src->val_size);
    map_clone_into(src->string_keys, dst->string_keys, src->val_size);
    return dst;
}

void vulp_map_values(void* p, VulpVec* out) {
    out->data = nullptr;
    out->len = out->cap = 0;
    if (!p) return;
    const auto* m = static_cast<VulpMap*>(p);
    vulp_vec_alloc(out, m->size(), sizeof(char*), 0);
    auto* values = reinterpret_cast<char**>(out->data);
    int64_t i = 0;
    for (const auto& kv : m->signed_keys) values[i++] = kv.second;
    for (const auto& kv : m->unsigned_keys) values[i++] = kv.second;
    for (const auto& kv : m->string_keys) values[i++] = kv.second;
}

void vulp_map_keys(void* p, VulpVec* out) {
    out->data = nullptr;
    out->len = out->cap = 0;
    if (!p) return;
    const auto* m = static_cast<VulpMap*>(p);
    if (m->kind == VULP_KEY_STRING) {
        vulp_vec_alloc(out, static_cast<int64_t>(m->string_keys.size()), sizeof(VulpVec), 0);
        auto* keys = reinterpret_cast<VulpVec*>(out->data);
        int64_t i = 0;
        for (const auto& kv : m->string_keys) {
            vulp_str_from_bytes(&keys[i++], kv.first.data(), static_cast<int64_t>(kv.first.size()));
        }
        return;
    }
    vulp_vec_alloc(out, m->size(), sizeof(int64_t), 0);
    auto* keys = reinterpret_cast<int64_t*>(out->data);
    int64_t i = 0;
    if (m->kind == VULP_KEY_UNSIGNED) {
        for (const auto& kv : m->unsigned_keys) keys[i++] = static_cast<int64_t>(kv.first);
    } else {
        for (const auto& kv : m->signed_keys) keys[i++] = kv.first;
    }
}

/*** memo tables ***/

void* vulp_memo_get(void** slot, const int64_t key_size, const int64_t val_size, const void* key) {
    Memo* m = memo_slot(slot, key_size, val_size);
    if (m->count == 0) return nullptr;
    bool found;
    char* e = memo_find(m, key, found);
    return found ? e + MEMO_TAG + m->key_size : nullptr;
}

void vulp_memo_put(void** slot, const int64_t key_size, const int64_t val_size, const void* key, const void* val) {
    Memo* m = memo_slot(slot, key_size, val_size);
    if ((m->count + 1) * 2 > m->cap) memo_grow(m);
    bool found;
    char* e = memo_find(m, key, found);
    if (!found) {
        std::memcpy(e, &m->gen, sizeof m->gen);
        std::memcpy(e + MEMO_TAG, key, static_cast<size_t>(key_size));
        m->count++;
    }
    std::memcpy(e + MEMO_TAG + key_size, val, static_cast<size_t>(val_size));
}

void vulp_memo_clear(void* p) {
    if (!p) return;
    auto* m = static_cast<Memo*>(p);
    m->count = 0;
    if (++m->gen == 0) { // generation counter wrapped: really wipe the table once
        if (m->table) std::memset(m->table, 0, static_cast<size_t>(m->cap * m->entry_size));
        m->gen = 1;
    }
}

} // extern "C"

/*** test hooks ***/

void vulp_rt_test_set_input(const std::string& data) {
    test_input = true;
    test_input_data = data;
    test_input_pos = 0;
    in_pos = in_len = 0;
    in_eof = false;
}

void vulp_rt_test_capture_begin() {
    out_flush();
    capturing = true;
    captured.clear();
}

std::string vulp_rt_test_capture_end() {
    out_flush();
    capturing = false;
    return std::move(captured);
}
