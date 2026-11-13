#!/usr/bin/env bash
# Starts the terminal on http://localhost:8080, driven by the C++ engine.
# In Codespaces this port is forwarded privately to you.
#  1. builds and tests the engine if needed
#  2. fetches the last trading day's official NSE closes as starting prices (a few seconds)
#  3. in the background: one year of NSE daily history, connector data (minutes on first start),
#     and the adverse-media feed (NSE filings + GDELT), refreshed every 2 minutes
set -euo pipefail
cd "$(dirname "$0")/.."
# build only if update.sh has not already built the current version
if [ ! -x build/mc_server ] || [ ! -f web/reports/report.json ] || [ "$(git rev-parse HEAD)" != "$(cat build/.built_rev 2>/dev/null)" ]; then
  bash scripts/build_and_report.sh
fi
mkdir -p logs
# a restart or re-attach must not leave an old server holding the port
pkill -f "build/mc_server" 2>/dev/null && sleep 1 || true
python3 tools/fetch_latest.py --no-history > logs/fetch_latest.log 2>&1 \
  || echo "Could not fetch NSE closes (see logs/fetch_latest.log); the terminal will use labelled placeholder prices."
today=$(TZ=Asia/Kolkata date +%F)
if [ ! -f web/data/history/_index.json ] || ! grep -q "\"asof\": \"$(tail -n +1 data/latest/instruments.csv 2>/dev/null | grep -o 'closes on [0-9-]*' | cut -d' ' -f3)\"" web/data/history/_index.json 2>/dev/null; then
  nohup python3 tools/fetch_latest.py --days 370 > logs/history.log 2>&1 &
fi
if [ ! -f web/data/connector.json ] || [ ! -d web/data/links ] || [ "$(date -r web/data/connector.json +%F)" != "$(date +%F)" ]; then
  nohup python3 tools/build_connector.py > logs/connector.log 2>&1 &
fi
python3 tools/check_channels.py > logs/channels.log 2>&1 || echo "TV channel check failed (see logs/channels.log)"
if [ ! -d web/data/fund ] || [ -z "$(find web/data/fund -name '*.json' -mtime -1 2>/dev/null | head -1)" ]; then
  pkill -f "tools/fetch_fundamentals.py" 2>/dev/null || true
  nohup python3 tools/fetch_fundamentals.py > logs/fundamentals.log 2>&1 &
fi
pkill -f "tools/fetch_news.py" 2>/dev/null || true
nohup python3 tools/fetch_news.py --loop 120 > logs/news.log 2>&1 &
echo "Background jobs: price history -> logs/history.log, connector -> logs/connector.log, news (every 2 min) -> logs/news.log ($today)"
DATA_ARGS=()
if [ -d data/nse ] && ls data/nse/*.csv >/dev/null 2>&1; then DATA_ARGS=(--data data/nse); fi
exec ./build/mc_server --web web --port "${MC_PORT:-8080}" "${DATA_ARGS[@]}"
