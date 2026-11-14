#!/usr/bin/env bash
# Starts the terminal on http://localhost:8080, driven by the C++ engine. Safe to run any number of times:
# if the engine of the current code version is already up, it does nothing.
# In Codespaces this port is forwarded privately to you.
#  1. builds and tests the engine if needed
#  2. fetches the last NSE trading day (open, high, low, close, volume, tick sizes) for the replay
#  3. in the background: price history, connector data, TV channel list, fundamentals, news (every 2 min)
# The demo account (wallet, positions, orders) is saved in data/session/state.tsv and restored on start.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p logs
PORT="${MC_PORT:-8080}"
rev=$(git rev-parse HEAD)
if pgrep -f "build/mc_server" >/dev/null 2>&1 && [ "$(cat logs/server.rev 2>/dev/null)" = "$rev" ] \
   && curl -fsS -m 3 "localhost:$PORT/api/state?lite=1" >/dev/null 2>&1; then
  echo "Engine already running (version ${rev:0:7}) on port $PORT."
  exit 0
fi
# build only if update.sh has not already built the current version
if [ ! -x build/mc_server ] || [ ! -f web/reports/report.json ] || [ "$rev" != "$(cat build/.built_rev 2>/dev/null)" ]; then
  bash scripts/build_and_report.sh
fi
# a restart must not leave an old server holding the port (its account is already saved to disk)
pkill -f "build/mc_server" 2>/dev/null && sleep 1 || true
python3 tools/fetch_latest.py --no-history > logs/fetch_latest.log 2>&1 \
  || echo "Could not fetch the NSE day (see logs/fetch_latest.log); the terminal uses the last fetched day, or labelled placeholders."
# replay day written by fetch_latest (7th column of the first data row)
day=$(awk -F, '!/^#/ && !/^symbol,/ && NF>=7 {print $7; exit}' data/latest/instruments.csv 2>/dev/null || true)
if [ -z "$day" ] || ! grep -q "\"asof\": \"$day\"" web/data/history/_index.json 2>/dev/null; then
  pkill -f "tools/fetch_latest.py --days" 2>/dev/null || true
  nohup python3 tools/fetch_latest.py --days 370 > logs/history.log 2>&1 &
fi
if [ ! -f web/data/connector.json ] || [ ! -d web/data/links ] || [ "$(date -r web/data/connector.json +%F)" != "$(date +%F)" ]; then
  pkill -f "tools/build_connector.py" 2>/dev/null || true
  nohup python3 tools/build_connector.py > logs/connector.log 2>&1 &
fi
pkill -f "tools/check_channels.py" 2>/dev/null || true
nohup python3 tools/check_channels.py > logs/channels.log 2>&1 &
if [ ! -d web/data/fund ] || [ -z "$(find web/data/fund -name '*.json' -mtime -1 2>/dev/null | head -1)" ]; then
  pkill -f "tools/fetch_fundamentals.py" 2>/dev/null || true
  nohup python3 tools/fetch_fundamentals.py > logs/fundamentals.log 2>&1 &
fi
pkill -f "tools/fetch_news.py" 2>/dev/null || true
nohup python3 tools/fetch_news.py --loop 120 > logs/news.log 2>&1 &
echo "Background jobs: history, connector, channels, fundamentals, news -> logs/ (replay day ${day:-unknown})"
DATA_ARGS=()
if [ -d data/nse ] && ls data/nse/*.csv >/dev/null 2>&1; then DATA_ARGS=(--data data/nse); fi
echo "$rev" > logs/server.rev
exec ./build/mc_server --web web --port "$PORT" --state data/session/state.tsv "${DATA_ARGS[@]}"
