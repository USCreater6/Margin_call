#!/usr/bin/env bash
# Starts the engine in the background, detached from any terminal or browser tab, so it keeps running
# while the Codespace runs. Called on every Codespace start (update.sh) and every attach. Idempotent.
cd "$(dirname "$0")/.."
mkdir -p logs
setsid nohup bash scripts/serve.sh >> logs/serve.log 2>&1 < /dev/null &
for i in $(seq 1 90); do   # wait up to 3 minutes for the first start (a first build can take a while)
  if curl -fsS -m 2 "localhost:${MC_PORT:-8080}/api/state?lite=1" >/dev/null 2>&1; then echo "Margin_call engine is up on port ${MC_PORT:-8080}."; exit 0; fi
  sleep 2
done
echo "Engine not up yet; see logs/serve.log"
