#!/usr/bin/env bash
# Build Release + Debug (ASan/UBSan) and run every test suite.
set -euo pipefail
cd "$(dirname "$0")/.."
PY=${PYTHON:-$PWD/.venv/bin/python}
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTSQ_BUILD_PYTHON=ON -DPython_EXECUTABLE="$PY" >/dev/null
cmake --build build
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build build-debug
./build/tsq_tests --gtest_brief=1
./build-debug/tsq_tests --gtest_brief=1
if [ -f python/tsq/__init__.py ] && [ "${SKIP_PY:-0}" != 1 ]; then
  "$PY" -m ruff check python tests/python bench
  "$PY" -m pytest -q
fi
echo "ALL CHECKS PASSED"
