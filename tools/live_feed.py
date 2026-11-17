#!/usr/bin/env python3
"""Live prices for the terminal during market hours. SIMULATION input only: no orders go anywhere.

Source choice (owner decision 16-17 Nov 2026): if the Codespaces secret UPSTOX_ANALYTICS_TOKEN is set, prices come
from Upstox (tools/upstox_feed.py: last price every 4 s, best bid/ask, NIFTY 50, SENSEX, MCX). Without it, the free
NSE server below is used as before.

Source: NSE's "cm-market" data server (Model Context Protocol over HTTPS, https://mcp.nseindia.in/cmmkt/mcp). Free and
without an account. Its own description: stock and index values refreshed every 1 minute; its page says intraday
data is delayed 1-3 minutes and is "not meant for real-time trading" (informational / educational use). The owner
approved using it for this private demo (16 Nov 2026).

Writes data/live/quotes.tsv (read by the engine every second):
    LIVE <fetched unix ms> <1|0> <message>
    X    NIFTY 50  <value> <previous close>
    S    <symbol>  <last price> <previous close>
and keeps the last raw answer of each tool in data/live/raw_<tool>.json, so the field names can be checked.

Outside 09:10-15:35 IST on weekdays it only writes "LIVE <ms> 0 market closed" (the engine then runs the replay).
Field names of the answers could not be seen before the first market session (the server returns no data while
the market is closed), so the parser accepts several common spellings and says so in the message if none matched.

Usage: live_feed.py [--loop 30] [--upstox-loop 4] [--once]
"""
import argparse, datetime as dt, json, os, sys, time, urllib.error, urllib.request

URL = "https://mcp.nseindia.in/cmmkt/mcp"
OUT = "data/live"
IST = dt.timezone(dt.timedelta(hours=5, minutes=30))
SYM_KEYS = ("symbol", "Symbol", "SYMBOL", "tradingSymbol", "TckrSymb", "sym")
PX_KEYS = ("ltp", "LTP", "lastPrice", "last_price", "lastTradedPrice", "last", "LastPrice", "lastPriceValue")
PREV_KEYS = ("previousClose", "prevClose", "previous_close", "prev_close", "pClose", "PrevClose", "previousClosePrice", "prevclose")
IDX_PX_KEYS = ("last", "lastValue", "last_value", "value", "ltp", "lastPrice", "current", "indexValue")


def now_ist():
    return dt.datetime.now(IST)


class Mcp:
    def __init__(self):
        self.sid, self.n = None, 0

    def _post(self, body, timeout=30):
        h = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream", "User-Agent": "Margin_call-demo/1.0"}
        if self.sid:
            h["Mcp-Session-Id"] = self.sid
        req = urllib.request.Request(URL, data=json.dumps(body).encode(), headers=h, method="POST")
        with urllib.request.urlopen(req, timeout=timeout) as r:
            sid = r.headers.get("Mcp-Session-Id")
            if sid:
                self.sid = sid
            return r.read().decode("utf-8", "replace")

    @staticmethod
    def _parse(text):
        """JSON-RPC answer, plain or as server-sent events (data: lines)."""
        t = text.strip()
        if t.startswith("{"):
            return json.loads(t)
        for line in t.splitlines():
            if line.startswith("data:"):
                return json.loads(line[5:])
        raise ValueError("unexpected answer: " + t[:200])

    def start(self):
        self.sid = None
        self._post({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                    "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "margin-call-demo", "version": "1"}}})
        self._post({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def call(self, name, args):
        self.n += 1
        ans = self._parse(self._post({"jsonrpc": "2.0", "id": 10 + self.n, "method": "tools/call", "params": {"name": name, "arguments": args}}))
        if "error" in ans:
            raise RuntimeError(f"{name}: {ans['error']}")
        txt = "".join(c.get("text", "") for c in ans.get("result", {}).get("content", []) if c.get("type") == "text")
        try:
            return json.loads(txt)
        except ValueError:
            return {"_text": txt}


def first(d, keys):
    for k in keys:
        if k in d and d[k] not in (None, "", "-"):
            try:
                return float(str(d[k]).replace(",", ""))
            except ValueError:
                pass
    return None


def rows_of(obj):
    """Every dict in the answer that carries a symbol key (searched through lists and nested objects)."""
    out = []
    def walk(x):
        if isinstance(x, dict):
            if any(k in x for k in SYM_KEYS):
                out.append(x)
            else:
                for v in x.values():
                    walk(v)
        elif isinstance(x, list):
            for v in x:
                walk(v)
    walk(obj)
    return out


def wanted():
    """Symbols the terminal shows: original watchlist, NIFTY 50, the owner's watched list."""
    s = set()
    try:
        for line in open("data/latest/instruments.csv"):
            if line and not line.startswith("#") and not line.startswith("symbol,"):
                s.add(line.split(",")[0])
    except OSError:
        pass
    for f in ("data/latest/nifty50.txt", "data/session/watch.txt"):
        try:
            s.update(x.strip() for x in open(f) if x.strip())
        except OSError:
            pass
    s.discard("NIFTY-FUT")
    return sorted(x for x in s if " " not in x)   # options are not on this server


def save_raw(name, obj):
    try:
        json.dump(obj, open(os.path.join(OUT, f"raw_{name}.json"), "w"), indent=1, default=str)
    except OSError:
        pass


def write(ok, msg, nifty=None, quotes=None):
    os.makedirs(OUT, exist_ok=True)
    lines = [f"LIVE\t{int(time.time() * 1000)}\t{1 if ok else 0}\t{msg.replace(chr(9), ' ').replace(chr(10), ' ')}"]
    if nifty:
        lines.append(f"X\tNIFTY 50\t{nifty[0]}\t{nifty[1] or 0}")
    for sym, (p, pc) in sorted((quotes or {}).items()):
        lines.append(f"S\t{sym}\t{p}\t{pc or 0}")
    tmp = os.path.join(OUT, "quotes.tsv.tmp")
    open(tmp, "w").write("\n".join(lines) + "\n")
    os.replace(tmp, os.path.join(OUT, "quotes.tsv"))


def market_window(t):
    m = t.hour * 60 + t.minute
    return t.weekday() < 5 and 9 * 60 + 10 <= m <= 15 * 60 + 35


def cycle(mcp):
    want = wanted()
    mcp.start()
    quotes, notes, nifty = {}, [], None
    # NIFTY 50 index
    try:
        a = mcp.call("cm_get_index_quote", {"indexName": "NIFTY 50"})
        save_raw("index", a)
        if isinstance(a, dict) and a.get("error"):
            notes.append("index: " + str(a["error"])[:120])
        else:
            src = a.get("data", a) if isinstance(a, dict) else {}
            v = first(src, IDX_PX_KEYS) if isinstance(src, dict) else None
            pc = first(src, PREV_KEYS) if isinstance(src, dict) else None
            if v:
                nifty = (v, pc)
            else:
                notes.append("index: value field not recognised (see data/live/raw_index.json)")
    except Exception as e:  # network, Akamai "Access Denied", JSON
        notes.append(f"index: {type(e).__name__} {str(e)[:120]}")
    # stocks: one call per first letter of the wanted symbols (up to 500 stocks per call)
    unread = 0
    for letter in sorted({s[0] for s in want}):
        try:
            a = mcp.call("cm_get_equity_stocks", {"limit": 500, "symbolFilter": letter})
            save_raw("equity_" + letter, a)
            if isinstance(a, dict) and a.get("error"):
                notes.append(f"stocks {letter}: {str(a['error'])[:100]}"); continue
            for r in rows_of(a):
                sym = next(str(r[k]).strip() for k in SYM_KEYS if k in r)
                if sym not in want:
                    continue
                p = first(r, PX_KEYS)
                if p and p > 0:
                    quotes[sym] = (p, first(r, PREV_KEYS))
                else:
                    unread += 1
        except Exception as e:
            notes.append(f"stocks {letter}: {type(e).__name__} {str(e)[:100]}")
        time.sleep(0.5)
    if unread and not quotes:
        notes.append("stock price field not recognised (see data/live/raw_equity_*.json)")
    ok = bool(quotes) or bool(nifty)
    msg = f"{len(quotes)} of {len(want)} stocks" + (", NIFTY 50 " + str(nifty[0]) if nifty else ", no NIFTY 50") + ("; " + "; ".join(notes[:4]) if notes else "")
    write(ok, msg, nifty, quotes)
    return msg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--loop", type=int, default=30, help="seconds between fetches in market hours")
    ap.add_argument("--once", action="store_true", help="one fetch now, whatever the time")
    ap.add_argument("--upstox-loop", type=int, default=4, help="seconds between Upstox fetches in NSE hours")
    a = ap.parse_args()
    if os.environ.get("UPSTOX_ANALYTICS_TOKEN", "").strip():
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import upstox_feed
        upstox_feed.run(a.upstox_loop, a.once, log=lambda m: print(m, flush=True))
        return
    print("UPSTOX_ANALYTICS_TOKEN is not set: using the free NSE server", flush=True)
    mcp = Mcp()
    if a.once:
        print(cycle(mcp)); return
    while True:
        t = now_ist()
        if market_window(t):
            try:
                print(t.strftime("%H:%M:%S"), cycle(mcp), flush=True)
            except Exception as e:
                write(False, f"{type(e).__name__}: {str(e)[:200]}")
                print(t.strftime("%H:%M:%S"), "error", e, flush=True)
            time.sleep(a.loop)
        else:
            write(False, "market closed (09:15-15:30 IST, Monday to Friday)")
            time.sleep(60)


if __name__ == "__main__":
    main()
