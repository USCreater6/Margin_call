#!/usr/bin/env python3
"""Real 1-minute candles of one NSE trading day for the replay (owner request 17 Nov 2026: demo trading on today's
data). SIMULATION input only: no orders go anywhere.

Source: Upstox candle APIs (verified 17 Nov 2026 from this repo's sandbox: they answer without a token; the
Analytics Token is sent when UPSTOX_ANALYTICS_TOKEN is set):
  today       GET https://api.upstox.com/v3/historical-candle/intraday/{key}/minutes/1
  past day    GET https://api.upstox.com/v3/historical-candle/{key}/minutes/1/{to}/{from}
  daily       GET https://api.upstox.com/v3/historical-candle/{key}/days/1/{to}/{from}   (previous close)
Each candle: [timestamp, open, high, low, close, volume, open interest], newest first.

Which day: today once its session is complete (15:29 candle present, after 15:30 IST); otherwise the last trading day
before today (Upstox's daily NIFTY 50 candles say which). Symbols: data/latest/instruments.csv, nifty50.txt,
data/session/watch.txt, config/watchlists.json; NIFTY-FUT = Upstox's nearest NIFTY future.

Writes data/latest/intraday/SYMBOL.csv (read by the engine at start):
  # date=YYYY-MM-DD pc=<previous close> source=<text>
  HH:MM,open,high,low,close,volume
and data/latest/intraday/_date. Skips the download when that day is already there (use --force to redo).

Also (owner decision 18 Nov 2026, chart ranges up to 5Y and the next expiry in the index strip):
  web/data/history5/SYMBOL.csv  5 years of daily candles (YYYYMMDD,open,high,low,close,volume), Upstox, once a day
  web/data/expiries.json        nearest option expiries of NIFTY, BANKNIFTY (NSE) and SENSEX (BSE), Upstox file

Usage: fetch_intraday.py [--force] [--date YYYY-MM-DD] [--no-daily]
"""
import argparse, datetime as dt, json, os, shutil, sys, time, urllib.error, urllib.parse, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upstox_feed as U

API = "https://api.upstox.com/v3/historical-candle"
OUT = "data/latest/intraday"
NIFTY = "NSE_INDEX|Nifty 50"


def get(path, token):
    h = {"Accept": "application/json", "User-Agent": "Margin_call-demo/1.0"}
    if token:
        h["Authorization"] = "Bearer " + token
    url = API + "/" + urllib.parse.quote(path, safe="/")
    for attempt in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=30) as r:
                a = json.loads(r.read().decode("utf-8", "replace"))
            if a.get("status") != "success":
                raise RuntimeError(str(a.get("errors"))[:200])
            return (a.get("data") or {}).get("candles") or []
        except urllib.error.HTTPError as e:
            if e.code == 429 and attempt < 3:
                time.sleep(2 ** attempt * 2)
                continue
            if e.code in (401, 403) and token:      # a bad token must not stop the open candle endpoints
                token, h = "", {k: v for k, v in h.items() if k != "Authorization"}
                continue
            raise RuntimeError(f"HTTP {e.code} {e.read().decode('utf-8', 'replace')[:150]}")
    return []


def day_of(c):
    return c[0][:10]


def symbols():
    s = U.wanted()                                   # instruments.csv, NIFTY 50, watch list, config lists
    try:
        if any(l.split(",")[0] == "NIFTY-FUT" for l in open("data/latest/instruments.csv")):
            s.append("NIFTY-FUT")
    except OSError:
        pass
    return s


def nifty_future(files, date_ms):
    best = None
    for x in files.get("NSE", []):
        if x.get("segment") == "NSE_FO" and x.get("instrument_type") == "FUT" and x.get("underlying_symbol") == "NIFTY" \
                and (x.get("expiry") or 0) >= date_ms and (best is None or x["expiry"] < best["expiry"]):
            best = x
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--date", default="")
    ap.add_argument("--no-daily", action="store_true")
    a = ap.parse_args()
    token = os.environ.get("UPSTOX_ANALYTICS_TOKEN", "").strip()
    now = U.now_ist()
    today = now.date().isoformat()
    log = lambda m: print(now.strftime("%H:%M:%S"), m, flush=True)
    # which day
    if a.date:
        D, mode = a.date, ("intraday" if a.date == today else "past")
    else:
        D, mode = None, "past"
        try:
            c = get(f"intraday/{NIFTY}/minutes/1", token)
            if c and day_of(c[0]) == today and c[0][0][11:16] == "15:29" and (now.hour, now.minute) >= (15, 30):
                D, mode = today, "intraday"
        except Exception as e:
            log(f"today's NIFTY 50 candles: {e}")
        if D is None:
            to = (now.date() - dt.timedelta(days=1)).isoformat()
            fr = (now.date() - dt.timedelta(days=14)).isoformat()
            c = get(f"{NIFTY}/days/1/{to}/{fr}", token)
            if not c:
                raise SystemExit("no daily NIFTY 50 candles from Upstox: cannot tell the last trading day")
            D = max(day_of(x) for x in c)
    have = ""
    try:
        have = open(os.path.join(OUT, "_date")).read().strip()
    except OSError:
        pass
    if not a.no_daily:
        extras(token, log)
    if not a.force and have == D:
        log(f"1-minute candles of {D} already here ({OUT}); nothing to do")
        return
    log(f"fetching real 1-minute candles of {D} ({'today, session complete' if mode == 'intraday' else 'last trading day'})")
    files = U.load_instruments(log)
    eq = U.equity_map(files)
    syms = symbols()
    keys = {s: eq[s] for s in syms if s in eq}
    fut = nifty_future(files, int(dt.datetime.fromisoformat(D).replace(tzinfo=U.IST).timestamp() * 1000))
    if "NIFTY-FUT" in syms and fut:
        keys["NIFTY-FUT"] = fut["instrument_key"]
    missing = [s for s in syms if s not in keys]
    tmp = OUT + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    prev_to = (dt.date.fromisoformat(D) - dt.timedelta(days=1)).isoformat()
    prev_fr = (dt.date.fromisoformat(D) - dt.timedelta(days=14)).isoformat()
    ok, bad = 0, []
    for s, k in sorted(keys.items()):
        try:
            c = get(f"intraday/{k}/minutes/1", token) if mode == "intraday" else get(f"{k}/minutes/1/{D}/{D}", token)
            c = [x for x in c if day_of(x) == D and "09:15" <= x[0][11:16] <= "15:29"]
            time.sleep(0.12)
            pd = get(f"{k}/days/1/{prev_to}/{prev_fr}", token)
            time.sleep(0.12)
            pc = max(pd, key=lambda x: x[0])[4] if pd else 0
            if len(c) < 188:                         # fewer than half the minutes: the engine keeps the official day
                bad.append(f"{s} ({len(c)} min)")
                continue
            src = "Upstox 1-minute candles" + (f" of {fut['trading_symbol']}" if s == "NIFTY-FUT" and fut else "")
            with open(os.path.join(tmp, s + ".csv"), "w") as f:
                f.write(f"# date={D} pc={pc} source={src.replace(' ', '_')}\n")
                for x in sorted(c, key=lambda x: x[0]):
                    f.write(f"{x[0][11:16]},{x[1]},{x[2]},{x[3]},{x[4]},{int(x[5] or 0)}\n")
            ok += 1
        except Exception as e:
            bad.append(f"{s} ({type(e).__name__} {str(e)[:60]})")
    open(os.path.join(tmp, "_date"), "w").write(D + "\n")
    shutil.rmtree(OUT, ignore_errors=True)
    os.replace(tmp, OUT)
    log(f"{ok} of {len(syms)} instruments on real 1-minute candles of {D}"
        + (f"; no Upstox key: {', '.join(missing[:8])}" if missing else "") + (f"; not used: {', '.join(bad[:8])}" if bad else ""))


def extras(token, log):
    """Next option expiries (index strip) and 5 years of daily candles (chart ranges 3M-5Y), at most once a day."""
    files = U.load_instruments(log)
    now_ms = int(time.time() * 1000)
    ex = {}
    for name, src in (("NIFTY 50", "NSE"), ("NIFTY BANK", "NSE"), ("SENSEX", "BSE")):
        und = {"NIFTY 50": "NIFTY", "NIFTY BANK": "BANKNIFTY", "SENSEX": "SENSEX"}[name]
        e = sorted({x["expiry"] for x in files.get(src, []) if x.get("instrument_type") in ("CE", "PE") and x.get("underlying_symbol") == und and (x.get("expiry") or 0) >= now_ms})
        if e:
            ex[name] = dt.datetime.fromtimestamp(e[0] / 1000, U.IST).strftime("%Y-%m-%d")
    os.makedirs("web/data", exist_ok=True)
    json.dump({"source": "Upstox instrument files", "made": U.now_ist().isoformat(timespec="seconds"), "next": ex}, open("web/data/expiries.json", "w"))
    out = "web/data/history5"
    stamp = os.path.join(out, "_date")
    today = U.now_ist().date().isoformat()
    try:
        if open(stamp).read().strip() == today:
            return
    except OSError:
        pass
    os.makedirs(out, exist_ok=True)
    eq = U.equity_map(files)
    to, fr = today, (U.now_ist().date() - dt.timedelta(days=5 * 365 + 2)).isoformat()
    n = 0
    for s in symbols():
        k = eq.get(s)
        if not k:
            continue
        try:
            c = get(f"{k}/days/1/{to}/{fr}", token)
            time.sleep(0.12)
        except Exception as e:
            log(f"5-year daily {s}: {e}")
            continue
        if c:
            with open(os.path.join(out, s + ".csv"), "w") as f:
                f.write("# date,open,high,low,close,volume (Upstox daily candles)\n")
                for x in sorted(c, key=lambda x: x[0]):
                    f.write(f"{x[0][:10].replace('-', '')},{x[1]},{x[2]},{x[3]},{x[4]},{int(x[5] or 0)}\n")
            n += 1
    open(stamp, "w").write(today + "\n")
    log(f"5 years of daily candles for {n} symbols (web/data/history5); next expiries {ex}")


if __name__ == "__main__":
    main()
