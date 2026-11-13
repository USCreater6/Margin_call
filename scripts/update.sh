#!/usr/bin/env bash
# Runs every time the Codespace starts: pulls the latest code from GitHub, rebuilds and re-tests it.
# Only fast-forward pulls are made, so nothing local is ever overwritten; data folders are not in git.
set -uo pipefail
cd "$(dirname "$0")/.."
mkdir -p logs
{
  echo "== update $(date -u +%FT%TZ)"
  before=$(git rev-parse --short HEAD)
  if git pull --ff-only origin main; then
    echo "code: $before -> $(git rev-parse --short HEAD)"
  else
    echo "WARNING: could not pull the latest code (local changes or no network); running the current version $before"
  fi
  bash scripts/build_and_report.sh && echo "build and tests: OK" || echo "BUILD OR TESTS FAILED: see this log"
} 2>&1 | tee -a logs/update.log
