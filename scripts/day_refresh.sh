#!/usr/bin/env bash
# Evening refresh (owner request, 17 Nov 2026): after the NSE close, fetch the day's official files (prices, ticks,
# NIFTY options) as soon as NSE publishes them, then restart the engine so the replay uses today's day and is never a
# day behind. The demo account is saved every 2 s and after each action, and restored on start, so nothing is lost;
# the page shows its lost-connection banner for a few seconds.
# Runs in the background (started by serve.sh). Tries every 30 minutes from 16:00 to 22:00 IST on weekdays.
# The exact time NSE publishes the daily file is not fixed: [must verify]; the retries cover the evening.
cd "$(dirname "$0")/.."
mkdir -p logs
day_of_file() { awk -F, '!/^#/ && !/^symbol,/ && NF>=7 {print $7; exit}' data/latest/instruments.csv 2>/dev/null; }
while true; do
  today=$(TZ=Asia/Kolkata date +%F); hm=$(TZ=Asia/Kolkata date +%H%M); wd=$(TZ=Asia/Kolkata date +%u)
  if [ "$wd" -le 5 ] && [ "$hm" -ge 1600 ] && [ "$hm" -le 2200 ] && [ "$(day_of_file)" != "$today" ]; then
    echo "$(date -u +%FT%TZ) looking for the NSE file of $today (have $(day_of_file))"
    python3 tools/fetch_latest.py --no-history > logs/fetch_latest.log 2>&1 || true
    if [ "$(day_of_file)" = "$today" ]; then
      python3 tools/fetch_options.py > logs/fetch_options.log 2>&1 || true
      echo "$(date -u +%FT%TZ) got $today: restarting the engine on the new day"
      rm -f logs/server.rev                      # makes serve.sh restart the engine
      setsid nohup bash scripts/serve.sh >> logs/serve.log 2>&1 < /dev/null &
      exit 0
    fi
    echo "$(date -u +%FT%TZ) not published yet; next try in 30 minutes"
  fi
  sleep 1800
done
