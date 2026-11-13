#!/usr/bin/env bash
# Serves the terminal, driven by the C++ engine, on http://localhost:8080.
# In Codespaces this port is forwarded privately to you.
set -euo pipefail
cd "$(dirname "$0")/.."
[ -x build/mc_server ] && [ -f web/reports/report.json ] || bash scripts/build_and_report.sh
DATA_ARGS=()
if [ -d data/nse ] && ls data/nse/*.csv >/dev/null 2>&1; then DATA_ARGS=(--data data/nse); fi
exec ./build/mc_server --web web --port "${MC_PORT:-8080}" "${DATA_ARGS[@]}"
