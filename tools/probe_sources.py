"""Diagnostic only (overwritten per probe): where does NSE publish each instrument's tick size?
Prints any field whose name contains 'tick' from NSE quote APIs. Nothing is saved."""
import json, sys, os, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nse_session import Nse

nse = Nse()


def walk(o, path=""):
    if isinstance(o, dict):
        for k, v in o.items():
            p = f"{path}.{k}" if path else k
            if "tick" in k.lower() or "lot" in k.lower():
                print(f"   {p} = {json.dumps(v)[:120]}")
            walk(v, p)
    elif isinstance(o, list):
        for i, v in enumerate(o[:3]):
            walk(v, f"{path}[{i}]")


for sym in ["RELIANCE", "HDFCBANK", "TCS", "INFY", "GOLDBEES"]:
    url = "https://www.nseindia.com/api/quote-equity?symbol=" + urllib.request.quote(sym)
    try:
        j = nse.get_json(url, referer="https://www.nseindia.com/get-quotes/equity?symbol=" + sym)
        print(f"quote-equity {sym}: top keys {list(j.keys())[:12]}")
        walk(j)
    except Exception as e:
        print(f"quote-equity {sym}: ERROR {e}")
try:
    j = nse.get_json("https://www.nseindia.com/api/quote-derivative?symbol=NIFTY", referer="https://www.nseindia.com/get-quotes/derivatives?symbol=NIFTY")
    print("quote-derivative NIFTY: top keys", list(j.keys())[:12])
    walk(j)
except Exception as e:
    print("quote-derivative NIFTY: ERROR", e)
