"""Diagnostic only (overwritten per probe): which free sources give the last trading day's INTRADAY path?
Prints point counts, first/last timestamps and the price range per source. Nothing is saved."""
import json, sys, os, urllib.request, datetime as dt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nse_session import Nse, UA

nse = Nse()
REF = "https://www.nseindia.com/get-quotes/equity?symbol=RELIANCE"


def show(tag, pts):
    if not pts:
        print(f"{tag}: 0 points"); return
    t0, t1 = pts[0][0], pts[-1][0]
    f = lambda t: dt.datetime.utcfromtimestamp(t / 1000 if t > 1e11 else t).strftime("%Y-%m-%d %H:%M:%S UTC")
    vals = [p[1] for p in pts if p[1] is not None]
    gaps = sorted({round((pts[i][0] - pts[i - 1][0]) / (1000 if t0 > 1e11 else 1)) for i in range(1, min(len(pts), 400))})[:6]
    print(f"{tag}: {len(pts)} points  {f(t0)} .. {f(t1)}  low {min(vals)} high {max(vals)}  step seconds {gaps}")


for ident in ["RELIANCEEQN", "TCSEQN", "NIFTY 50", "GOLDBEESEQN"]:
    url = "https://www.nseindia.com/api/chart-databyindex?index=" + urllib.request.quote(ident)
    try:
        j = nse.get_json(url, referer=REF)
        show(f"NSE chart-databyindex {ident}", j.get("grapthData") or j.get("graphData") or [])
        print("   keys:", list(j.keys())[:8])
    except Exception as e:
        print(f"NSE chart-databyindex {ident}: ERROR {e}")

for ident in ["RELIANCEEQN"]:
    url = "https://www.nseindia.com/api/chart-databyindex?index=" + urllib.request.quote(ident) + "&preopen=true"
    try:
        j = nse.get_json(url, referer=REF)
        show(f"NSE chart-databyindex preopen {ident}", j.get("grapthData") or [])
    except Exception as e:
        print(f"NSE preopen {ident}: ERROR {e}")

# NIFTY futures intraday on NSE (identifier format unknown: try candidates)
for ident in ["FUTIDXNIFTY25-11-2026XX0.00", "FUTIDXNIFTY24-11-2026XX0.00", "FUTIDXNIFTY26-11-2026XX0.00"]:
    url = "https://www.nseindia.com/api/chart-databyindex?index=" + urllib.request.quote(ident)
    try:
        j = nse.get_json(url, referer="https://www.nseindia.com/get-quotes/derivatives?symbol=NIFTY")
        show(f"NSE chart-databyindex {ident}", j.get("grapthData") or [])
    except Exception as e:
        print(f"NSE {ident}: ERROR {e}")

for sym in ["RELIANCE.NS", "^NSEI"]:
    url = f"https://query1.finance.yahoo.com/v8/finance/chart/{urllib.request.quote(sym)}?interval=1m&range=5d"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": UA}), timeout=30) as r:
            j = json.loads(r.read())
        res = j["chart"]["result"][0]
        ts, cl = res["timestamp"], res["indicators"]["quote"][0]["close"]
        show(f"Yahoo 1m {sym}", list(zip(ts, cl)))
    except Exception as e:
        print(f"Yahoo 1m {sym}: ERROR {e}")
