#!/bin/sh
# Naive recursive Fibonacci with and without `dp`: the only difference is the keyword.
set -e
cd "$(dirname "$0")"
VULPC=${VULPC:-../../../../build/vulpc}
mkdir -p ../out
for f in fib_plain fib_dp; do
    "$VULPC" -O2 $f.vp -o ../out/$f
    /usr/bin/time -f "$f: %e s, %M KiB" ../out/$f
done
