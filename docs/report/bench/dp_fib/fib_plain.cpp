#include <cstdio>

long long fib(long long n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

int main() {
    long long n = 0;
    if (std::scanf("%lld", &n) != 1) return 2;
    std::printf("%lld\n", fib(n));
}
