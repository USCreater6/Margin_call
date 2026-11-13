#!/usr/bin/env bash
# Builds the C++ core, runs every test, then produces the SIMULATION report shown in the terminal.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMC_WERROR=ON
cmake --build build -j"$(nproc)"
./build/mc_tests
DATA_DIR="${MC_DATA_DIR:-data/synthetic}"
if [ ! -d "$DATA_DIR" ] || [ -z "$(ls -A "$DATA_DIR" 2>/dev/null)" ]; then
  ./build/mc_gensynth data/synthetic 42
  DATA_DIR=data/synthetic
fi
./build/mc_backtest --data "$DATA_DIR" --out web/reports
./build/mc_bench web/reports/latency.json
