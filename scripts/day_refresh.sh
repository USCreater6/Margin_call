#!/usr/bin/env bash
# Evening refresh (owner requests, 17 Nov 2026): the replay is never a day behind.
#  - from 15:35 IST: today's real 1-minute candles (Upstox, tools/fetch_intraday.py) as soon as the session is complete
#  - from 16:00 IST: the day's official NSE file (prices, ticks, NIFTY options) once NSE publishes it
# After each new day the engine is restarted on it. The demo account is saved every 2 s and after each action, and
# restored on start, so nothing is lost; the page shows its lost-connection banner for a few seconds.
# Runs in the background (started by serve.sh); tries every 10 minutes until 22:00 IST on weekdays.
# The exact time NSE publishes its daily file is not fixed: [must verify]; the retries cover the evening.
cd "$(dirname "$0")/.."
mkdir -p logs
day_of_file() { awk -F, '!/^#/ && !/^symbol,/ && NF>=7 {print $7; exit}' data/latest/instruments.csv 2>/dev/null; }
candle_day() { cat data/latest/intraday/_date 2>/dev/null; }
restart() {
  echo "$(date -u +%FT%TZ) $1: restarting the engine on it"
  rm -f logs/server.rev                      # makes serve.sh restart the engine
  setsid nohup bash scripts/serve.sh >> logs/serve.log 2>&1 < /dev/null &
  exit 0
}
while true; do
  today=$(TZ=Asia/Kolkata date +%F); hm=$(TZ=Asia/Kolkata date +%H%M); wd=$(TZ=Asia/Kolkata date +%u)
  if [ "$wd" -le 5 ] && [ "$hm" -ge 1535 ] && [ "$hm" -le 2200 ]; then
    if [ "$(candle_day)" != "$today" ]; then
      timeout 300 python3 tools/fetch_intraday.py > logs/fetch_intraday.log 2>&1 || true
      [ "$(candle_day)" = "$today" ] && restart "got today's 1-minute candles ($today)"
    fi
    if [ "$hm" -ge 1600 ] && [ "$(day_of_file)" != "$today" ]; then
      python3 tools/fetch_latest.py --no-history > logs/fetch_latest.log 2>&1 || true
      if [ "$(day_of_file)" = "$today" ]; then
        python3 tools/fetch_options.py > logs/fetch_options.log 2>&1 || true
        restart "got the official NSE file of $today"
      fi
      echo "$(date -u +%FT%TZ) NSE file of $today not published yet; next try in 10 minutes"
    fi
  fi
  sleep 600
done
