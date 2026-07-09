#!/usr/bin/env bash
# Rebuild in Release and rerun the full benchmark. Usage:
#   bench/run_bench.sh [extra bench_queries args...]     e.g. --rows 50000000 --reps 7
# Output is tee'd to bench/out/bench-<timestamp>.txt (gitignored).
set -euo pipefail
cd "$(dirname "$0")/.."
# Separate build dir so the developer build in build/ keeps its configuration.
cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DTSQ_BUILD_PYTHON=OFF -DTSQ_BUILD_TESTS=OFF >/dev/null
cmake --build build-bench --target bench_queries
mkdir -p bench/out
out="bench/out/bench-$(date +%Y%m%d-%H%M%S).txt"
{
  echo "# date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "# host: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m), $(sysctl -n hw.ncpu 2>/dev/null || nproc) cpus, $(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1073741824 )) GB"
  echo "# os: $(uname -sr); llvm: $(/opt/homebrew/opt/llvm/bin/llvm-config --version 2>/dev/null || llvm-config --version)"
  echo "# load averages before run: $(uptime | sed 's/.*load averages*: //')"
  echo "# command: build-bench/bench_queries $*"
  ./build-bench/bench_queries "$@"
  echo "# load averages after run: $(uptime | sed 's/.*load averages*: //')"
} | tee "$out"
echo "saved to $out"
