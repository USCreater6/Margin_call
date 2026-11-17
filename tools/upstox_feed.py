#!/usr/bin/env python3
"""Live prices for the terminal from Upstox (read-only Analytics Token). SIMULATION input only: no orders go anywhere.

The token is read from the environment variable UPSTOX_ANALYTICS_TOKEN (a Codespaces secret, never in the repo).
The Analytics Token is read-only: it cannot place, modify or cancel orders (Upstox developer docs).

Upstox APIs used (all GET, verified 17 Nov 2026 against upstox.com/developer/api-documentation):
  instruments  https://assets.upstox.com/market-quote/instruments/exchange/{NSE,BSE,MCX}.json.gz  (public, daily)
               fields: segment, instrument_key, trading_symbol, instrument_type, tick_size (paise: 5.0 = 0.05),
               lot_size, expiry (ms), underlying_symbol
  full quote   https://api.upstox.com/v2/market-quote/quotes?instrument_key=K1,K2,...
               per instrument: last_price, volume, average_price, net_change, ohlc{open,high,low,close},
               depth{buy[5],sell[5]} {price,quantity,orders}, total_buy_quantity, total_sell_quantity,
               lower_circuit_limit, upper_circuit_limit, last_trade_time, instrument_token
  LTP V3       https://api.upstox.com/v3/market-quote/ltp?instrument_key=...  -> last_price, ltq, volume, cp (prev close)
Answers are keyed "EXCHANGE:SYMBOL"; each row carries instrument_token = the requested key, which is what we map on.

Writes (same format as before, so the engine is unchanged):
  data/live/quotes.tsv          LIVE <ms> <1|0> <message> / X NIFTY 50 <value> <prev> / S <symbol> <last> <prev>
  data/live/upstox_quotes.json  everything per instrument (bid/ask 5 levels, OHLC, volume, circuits) for the
                                market-watch grid and the index strip (NIFTY 50, NIFTY BANK, INDIA VIX, SENSEX,
                                MCX nearest futures)
  logs/live_feed.log (stdout)   one line per cycle; the token itself is never printed

Unknowns [must verify on the first market session]: maximum keys per full-quote call (500 assumed for LTP per the
docs; full quotes are sent in batches of 100 to stay well inside it) and the rate limit that applies to the
Analytics Token (the standard limits are 50/s, 500/min, 2,000/30 min; the default cycle of 4 s with 2 calls stays at
900 per 30 min).
"""
import datetime as dt, gzip, json, os, time, urllib.error, urllib.parse, urllib.request

API = "https://api.upstox.com"
ASSETS = "https://assets.upstox.com/market-quote/instruments/exchange/{ex}.json.gz"
OUT = "data/live"
CACHE = "data/upstox"
IST = dt.timezone(dt.timedelta(hours=5, minutes=30))
INDICES = {"NIFTY 50": "NSE_INDEX|Nifty 50", "NIFTY BANK": "NSE_INDEX|Nifty Bank", "INDIA VIX": "NSE_INDEX|India VIX",
           "SENSEX": "BSE_INDEX|SENSEX"}
MCX = ["GOLD", "SILVER", "CRUDEOIL", "NATURALGAS", "COPPER", "ZINC", "ALUMINIUM", "LEAD", "NICKEL", "MCXBULLDEX", "MCXMETLDEX"]
BATCH = 100


def now_ist():
    return dt.datetime.now(IST)


class AuthError(Exception):
    pass


class Upstox:
    def __init__(self, token):
        self.h = {"Accept": "application/json", "Authorization": "Bearer " + token, "User-Agent": "Margin_call-demo/1.0"}
        self.calls = 0

    def get(self, path, keys):
        url = API + path + "?instrument_key=" + urllib.parse.quote(",".join(keys), safe=",")
        req = urllib.request.Request(url, headers=self.h)
        self.calls += 1
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                ans = json.loads(r.read().decode("utf-8", "replace"))
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", "replace")[:300]
            if e.code in (401, 403):
                raise AuthError(f"HTTP {e.code}: token rejected (UPSTOX_ANALYTICS_TOKEN wrong, revoked or expired) {body}")
            raise RuntimeError(f"HTTP {e.code} {body}")
        if ans.get("status") != "success":
            raise RuntimeError("answer status " + str(ans.get("status")) + " " + json.dumps(ans.get("errors"))[:200])
        out = {}
        for row in (ans.get("data") or {}).values():   # keyed EXCHANGE:SYMBOL; map back on the requested key
            k = row.get("instrument_token")
            if k:
                out[k] = row
        return out


def load_instruments(log):
    """Daily instrument files (cached in data/upstox/, re-downloaded when older than 12 h)."""
    os.makedirs(CACHE, exist_ok=True)
    res = {}
    for ex in ("NSE", "BSE", "MCX"):
        p = os.path.join(CACHE, ex + ".json.gz")
        if not os.path.exists(p) or time.time() - os.path.getmtime(p) > 12 * 3600:
            try:
                with urllib.request.urlopen(urllib.request.Request(ASSETS.format(ex=ex), headers={"User-Agent": "Margin_call-demo/1.0"}), timeout=60) as r:
                    blob = r.read()
                open(p + ".tmp", "wb").write(blob)
                os.replace(p + ".tmp", p)
            except Exception as e:
                log(f"instrument file {ex}: {type(e).__name__} {str(e)[:120]}" + (" (using cached copy)" if os.path.exists(p) else ""))
        try:
            res[ex] = json.load(gzip.open(p))
        except Exception as e:
            log(f"instrument file {ex} unreadable: {type(e).__name__}")
            res[ex] = []
    return res


def equity_map(files):
    """NSE cash-market symbol -> instrument key (EQ first, then BE/BZ series for the same symbol)."""
    m = {}
    rank = {"EQ": 0, "BE": 1, "BZ": 2}
    for x in files.get("NSE", []):
        if x.get("segment") != "NSE_EQ":
            continue
        s, t = x.get("trading_symbol"), x.get("instrument_type")
        if t not in rank or not s:
            continue
        if s not in m or rank[t] < m[s][1]:
            m[s] = (x["instrument_key"], rank[t])
    return {s: v[0] for s, v in m.items()}


def mcx_nearest(files, today_ms):
    """Nearest unexpired future per MCX commodity -> (label, key, trading_symbol)."""
    best = {}
    for x in files.get("MCX", []):
        u = x.get("underlying_symbol")
        if x.get("instrument_type") != "FUT" or u not in MCX or (x.get("expiry") or 0) < today_ms:
            continue
        if u not in best or x["expiry"] < best[u]["expiry"]:
            best[u] = x
    return [(u, best[u]["instrument_key"], best[u]["trading_symbol"]) for u in MCX if u in best]


def wanted():
    """Symbols the terminal shows: original instruments, NIFTY 50, the owner's watch list and the DEFAULT tab."""
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
    try:
        for lst in json.load(open("config/watchlists.json")).get("lists", []):
            s.update(lst.get("symbols", []))
    except (OSError, ValueError):
        pass
    s.discard("NIFTY-FUT")
    return sorted(x for x in s if " " not in x)


def write_tsv(ok, msg, nifty=None, quotes=None):
    os.makedirs(OUT, exist_ok=True)
    lines = [f"LIVE\t{int(time.time() * 1000)}\t{1 if ok else 0}\t{msg.replace(chr(9), ' ').replace(chr(10), ' ')}"]
    if nifty:
        lines.append(f"X\tNIFTY 50\t{nifty[0]}\t{nifty[1] or 0}")
    for sym, (p, pc) in sorted((quotes or {}).items()):
        lines.append(f"S\t{sym}\t{p}\t{pc or 0}")
    tmp = os.path.join(OUT, "quotes.tsv.tmp")
    open(tmp, "w").write("\n".join(lines) + "\n")
    os.replace(tmp, os.path.join(OUT, "quotes.tsv"))


def write_json(obj):
    os.makedirs(OUT, exist_ok=True)
    tmp = os.path.join(OUT, "upstox_quotes.json.tmp")
    json.dump(obj, open(tmp, "w"), separators=(",", ":"))
    os.replace(tmp, os.path.join(OUT, "upstox_quotes.json"))


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def full_row(r):
    """Grid row from a full-quote answer. prev close = last - net_change (the answer's ohlc.close is not
    documented as the previous close)."""
    lp, nc = num(r.get("last_price")), num(r.get("net_change"))
    o = r.get("ohlc") or {}
    d = r.get("depth") or {}
    lv = lambda side: [[num(x.get("price")), int(x.get("quantity") or 0), int(x.get("orders") or 0)] for x in (d.get(side) or [])]
    return {"ltp": lp, "prev": (lp - nc) if lp is not None and nc is not None else None, "chg": nc,
            "open": num(o.get("open")), "high": num(o.get("high")), "low": num(o.get("low")),
            "vol": num(r.get("volume")), "avg": num(r.get("average_price")),
            "bids": lv("buy"), "asks": lv("sell"),
            "tbq": num(r.get("total_buy_quantity")), "tsq": num(r.get("total_sell_quantity")),
            "lc": num(r.get("lower_circuit_limit")), "uc": num(r.get("upper_circuit_limit")),
            "ltt": r.get("last_trade_time")}


def market_window(t):
    """NSE cash 09:15-15:30 (fetched 09:08-15:35); MCX runs to 23:30 or 23:55 (US daylight time) [must verify per
    MCX circular], so index strip data is refreshed to 23:59 on weekdays."""
    m = t.hour * 60 + t.minute
    nse = t.weekday() < 5 and 9 * 60 + 8 <= m <= 15 * 60 + 35
    mcx = t.weekday() < 5 and 9 * 60 <= m <= 23 * 60 + 59
    return nse, mcx


class Feed:
    def __init__(self, token, log):
        self.u, self.log = Upstox(token), log
        self.files, self.eq, self.mcx, self.day = {}, {}, [], None

    def refresh_instruments(self):
        d = now_ist().date()
        if self.day == d and self.eq:
            return
        self.files = load_instruments(self.log)
        self.eq = equity_map(self.files)
        self.mcx = mcx_nearest(self.files, int(time.time() * 1000))
        self.day = d
        self.log(f"instruments: {len(self.eq)} NSE cash symbols, MCX nearest futures: " + ", ".join(s for _, _, s in self.mcx))

    def check_token(self):
        """One LTP call: proves the token and the network path (works when the market is closed: last values)."""
        a = self.u.get("/v3/market-quote/ltp", [INDICES["NIFTY 50"]])
        r = a.get(INDICES["NIFTY 50"]) or {}
        return num(r.get("last_price")), num(r.get("cp"))

    def cycle(self, nse_open):
        self.refresh_instruments()
        want = wanted()
        keys = {s: self.eq[s] for s in want if s in self.eq}
        missing = [s for s in want if s not in self.eq]
        rows, notes = {}, []
        # stocks: full quote (bid/ask) in batches
        inv = {k: s for s, k in keys.items()}
        ks = list(inv)
        for i in range(0, len(ks), BATCH):
            try:
                for k, r in self.u.get("/v2/market-quote/quotes", ks[i:i + BATCH]).items():
                    if k in inv:
                        rows[inv[k]] = full_row(r)
            except AuthError:
                raise
            except Exception as e:
                notes.append(f"quotes: {type(e).__name__} {str(e)[:120]}")
        # indices + MCX: LTP V3 (last price, previous close)
        strip = {}
        idx_keys = dict(INDICES)
        idx_keys.update({f"MCX {u}": k for u, k, _ in self.mcx})
        try:
            a = self.u.get("/v3/market-quote/ltp", list(idx_keys.values()))
            for label, k in idx_keys.items():
                r = a.get(k)
                if r:
                    strip[label] = {"ltp": num(r.get("last_price")), "prev": num(r.get("cp")), "vol": num(r.get("volume")),
                                    "contract": next((s for u, kk, s in self.mcx if kk == k), None)}
        except AuthError:
            raise
        except Exception as e:
            notes.append(f"indices: {type(e).__name__} {str(e)[:120]}")
        quotes = {s: (r["ltp"], r["prev"]) for s, r in rows.items() if r["ltp"] and r["ltp"] > 0}
        n = strip.get("NIFTY 50") or {}
        nifty = (n["ltp"], n.get("prev")) if n.get("ltp") else None
        write_json({"source": "Upstox (Analytics Token, read-only)", "fetched": int(time.time() * 1000),
                    "nse_open": nse_open, "stocks": rows, "strip": strip, "missing": missing})
        b = rows.get("INFY") or next(iter(rows.values()), None)
        sample = ""
        if b and b["bids"] and b["asks"]:
            sym = "INFY" if "INFY" in rows else next(iter(rows))
            sample = f", {sym} bid {b['bids'][0][0]} x {b['bids'][0][1]} / ask {b['asks'][0][0]} x {b['asks'][0][1]}"
        msg = (f"Upstox: {len(quotes)} of {len(want)} stocks" + (f", NIFTY 50 {nifty[0]}" if nifty else ", no NIFTY 50")
               + sample + (f"; not on NSE list: {', '.join(missing[:5])}" if missing else "") + ("; " + "; ".join(notes[:3]) if notes else ""))
        # ok=1 only in NSE hours: the engine treats a fresh ok file as "live"; outside them it keeps the replay
        write_tsv(nse_open and bool(quotes or nifty), msg if nse_open else "market closed for NSE; " + msg, nifty, quotes)
        return msg


def run(loop, once, log=print):
    token = os.environ.get("UPSTOX_ANALYTICS_TOKEN", "").strip()
    if not token:
        raise SystemExit("UPSTOX_ANALYTICS_TOKEN is not set")
    f = Feed(token, lambda m: log(now_ist().strftime("%H:%M:%S") + " " + m))
    try:
        f.refresh_instruments()
        lp, cp = f.check_token()
        log(now_ist().strftime("%H:%M:%S") + f" Upstox token OK: NIFTY 50 last {lp}, previous close {cp}")
    except AuthError as e:
        write_tsv(False, "Upstox: " + str(e)[:200])
        log(now_ist().strftime("%H:%M:%S") + " Upstox " + str(e)[:300])
        if once:
            return
        time.sleep(300)
    except Exception as e:
        log(now_ist().strftime("%H:%M:%S") + f" Upstox check failed: {type(e).__name__} {str(e)[:200]}")
    if once:
        log(f.cycle(market_window(now_ist())[0]))
        return
    wait, last_log = max(loop, 2), 0.0
    while True:
        t = now_ist()
        nse, mcx = market_window(t)
        if nse or mcx:
            try:
                msg = f.cycle(nse)
                if time.time() - last_log >= (30 if nse else 600):   # keep the log short
                    log(t.strftime("%H:%M:%S") + " " + msg + f" (calls so far {f.u.calls})")
                    last_log = time.time()
                wait = max(loop, 2) if nse else 30      # after the NSE close only the index strip / MCX moves
            except AuthError as e:
                write_tsv(False, "Upstox: " + str(e)[:200])
                log(t.strftime("%H:%M:%S") + " " + str(e)[:300])
                wait = 300
            except Exception as e:
                write_tsv(False, f"Upstox: {type(e).__name__}: {str(e)[:200]}")
                log(t.strftime("%H:%M:%S") + f" error {type(e).__name__} {str(e)[:200]}")
                wait = min(wait * 2, 60)       # back off on errors (e.g. HTTP 429)
            time.sleep(wait)
        else:
            write_tsv(False, "market closed (NSE 09:15-15:30 IST, Monday to Friday); Upstox feed waiting")
            time.sleep(60)
