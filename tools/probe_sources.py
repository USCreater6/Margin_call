#!/usr/bin/env python3
"""Probe 8: NSE integrated-filing financial results (quarters from March 2025). Diagnostic only."""
import json, os, sys, urllib.parse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nse_session import Nse
nse = Nse()
for sym in ("RELIANCE", "HDFCBANK"):
    u = "https://www.nseindia.com/api/integrated-filing-results?" + urllib.parse.urlencode({"index": "equities", "type": "Integrated Filing- Financials", "page": 1, "size": 20, "symbol": sym})
    try:
        j = nse.get_json(u, "https://www.nseindia.com/companies-listing/corporate-integrated-filing")
        rows = j.get("data", []) if isinstance(j, dict) else j
        print(f"{sym}: top-level {type(j).__name__} keys={list(j.keys()) if isinstance(j, dict) else ''} rows={len(rows)}")
        for r in rows[:4]: print("  ", json.dumps(r)[:900])
    except Exception as e:
        print(sym, "error", e)
