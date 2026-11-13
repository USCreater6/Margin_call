#!/usr/bin/env bash
# Serves the terminal on http://localhost:8080 (in Codespaces this port is forwarded privately to you).
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f web/reports/report.json ] || bash scripts/build_and_report.sh
command -v python3 >/dev/null || { echo "python3 is required to serve the terminal"; exit 1; }
echo "Margin_call terminal (SIMULATION) on port 8080"
cd web && exec python3 -m http.server 8080 --bind 0.0.0.0
