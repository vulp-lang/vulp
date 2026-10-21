#!/bin/sh
# Cross-language Fibonacci benchmark for dp fn and plain recursion.
set -e
cd "$(dirname "$0")"
VULPC=${VULPC:-../../../../build/vulpc}
python3 benchmark.py --vulpc "$VULPC"
