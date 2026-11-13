#!/usr/bin/env bash
# Downloads NSE daily files into data/nse (not committed), then rebuilds the report on them.
# Usage: scripts/fetch_data.sh [START_DATE] [END_DATE]   e.g. scripts/fetch_data.sh 2019-01-01
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tools/fetch_nse.py --start "${1:-2019-01-01}" --end "${2:-$(date -u +%F)}" --out data/nse
MC_DATA_DIR=data/nse bash scripts/build_and_report.sh
