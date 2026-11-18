#!/usr/bin/env python3
"""Live prices every second from the Upstox market-data stream (owner decision 18 Nov 2026). SIMULATION input only:
no orders go anywhere. Started by tools/live_feed.py when the Codespaces secret UPSTOX_ANALYTICS_TOKEN is set.

Upstox Market Data Feed V3 (upstox.com/developer/api-documentation/v3/get-market-data-feed, checked 18 Nov 2026):
  GET https://api.upstox.com/v3/feed/market-data-feed/authorize  (Bearer token) -> data.authorized_redirect_uri (wss)
  subscribe: binary JSON {"guid", "method": "sub", "data": {"mode": "full", "instrumentKeys": [...]}}
  messages: protobuf FeedResponse (MarketDataFeedV3.proto; the compiled module comes with the official
  upstox-python-sdk package). "full" mode: last price, close, 5-level bid/ask, day OHLC, volume, average price.
  Standard limits: 2 connections per user, up to 2,000 instruments in "full" mode.
The Analytics Token is listed by Upstox as valid for Market Data Feed V3. [must verify] that the stream accepts it
from a Codespace; if the stream fails, this falls back to the 4-second quote calls (tools/upstox_feed.py).

Writes once a second (same files as tools/upstox_feed.py, so the engine and the page are unchanged):
  data/live/quotes.tsv          LIVE / X NIFTY 50 / S <symbol> <last> <prev close> <day volume>
  data/live/upstox_quotes.json  per instrument: last, previous close, open/high/low, volume, average price,
                                5-level bid/ask, totals; index strip
"""
import datetime as dt, json, os, ssl, threading, time, urllib.request, uuid
import upstox_feed as U

AUTH = "https://api.upstox.com/v3/feed/market-data-feed/authorize"


def deps_ok():
    try:
        import websocket  # noqa: F401  (websocket-client)
        from upstox_client.feeder.proto import MarketDataFeedV3_pb2  # noqa: F401
        return True
    except Exception:
        return False


class Stream:
    def __init__(self, token, log):
        import websocket
        from upstox_client.feeder.proto import MarketDataFeedV3_pb2 as pb
        self.websocket, self.pb, self.token, self.log = websocket, pb, token, log
        self.feed = U.Feed(token, log)            # instrument files, symbol lists and the 4-second fallback
        self.state, self.lock = {}, threading.Lock()
        self.msgs, self.last_msg, self.connected, self.err = 0, 0.0, False, ""
        self.keys, self.ws = {}, None

    # --- protobuf -> plain values ---
    def absorb(self, raw):
        r = self.pb.FeedResponse.FromString(raw)
        now = time.time()
        with self.lock:
            for key, f in r.feeds.items():
                s = self.state.setdefault(key, {})
                kind = f.WhichOneof("FeedUnion")
                if kind == "ltpc":
                    self._ltpc(s, f.ltpc)
                elif kind == "fullFeed":
                    ff = f.fullFeed
                    sub = ff.WhichOneof("FullFeedUnion")
                    m = ff.marketFF if sub == "marketFF" else ff.indexFF
                    self._ltpc(s, m.ltpc)
                    for o in m.marketOHLC.ohlc:
                        if o.interval == "1d":
                            s["open"], s["high"], s["low"] = o.open or None, o.high or None, o.low or None
                    if sub == "marketFF":
                        q = list(m.marketLevel.bidAskQuote)
                        s["bids"] = [[x.bidP, int(x.bidQ), None] for x in q if x.bidP > 0][:5]
                        s["asks"] = [[x.askP, int(x.askQ), None] for x in q if x.askP > 0][:5]
                        s["vol"], s["avg"], s["tbq"], s["tsq"] = float(m.vtt) or None, m.atp or None, m.tbq or None, m.tsq or None
                elif kind == "firstLevelWithGreeks":
                    self._ltpc(s, f.firstLevelWithGreeks.ltpc)
                s["t"] = now
            self.msgs += 1
            self.last_msg = now

    @staticmethod
    def _ltpc(s, x):
        if x.ltp > 0:
            s["ltp"] = x.ltp
        if x.cp > 0:
            s["prev"] = x.cp
        if x.ltt:
            s["ltt"] = str(x.ltt)

    # --- connection ---
    def authorize(self):
        req = urllib.request.Request(AUTH, headers={"Accept": "application/json", "Authorization": "Bearer " + self.token, "User-Agent": "Margin_call-demo/1.0"})
        with urllib.request.urlopen(req, timeout=20) as r:
            a = json.loads(r.read().decode("utf-8", "replace"))
        uri = ((a.get("data") or {}).get("authorized_redirect_uri") or (a.get("data") or {}).get("authorizedRedirectUri"))
        if not uri:
            raise RuntimeError("authorize: no stream address in the answer " + json.dumps(a)[:200])
        return uri

    def wanted_keys(self):
        self.feed.refresh_instruments()
        k = {s: self.feed.eq[s] for s in U.wanted() if s in self.feed.eq}
        k.update(U.INDICES)
        k.update({f"MCX {u}": key for u, key, _ in self.feed.mcx})
        return k

    def subscribe(self, keys):
        if self.ws and keys:
            msg = {"guid": str(uuid.uuid4()), "method": "sub", "data": {"mode": "full", "instrumentKeys": list(keys)[:1900]}}
            self.ws.send(json.dumps(msg).encode("utf-8"), opcode=self.websocket.ABNF.OPCODE_BINARY)

    def run_ws(self):
        uri = self.authorize()
        self.keys = self.wanted_keys()

        def on_open(ws):
            self.connected, self.err = True, ""
            self.subscribe(self.keys.values())
            self.log(f"stream connected: {len(self.keys)} instruments subscribed in full mode")

        def on_msg(ws, m):
            if isinstance(m, (bytes, bytearray)):
                try:
                    self.absorb(m)
                except Exception as e:
                    self.err = f"decode: {type(e).__name__} {str(e)[:100]}"

        def on_err(ws, e):
            self.err = f"{type(e).__name__} {str(e)[:150]}"

        def on_close(ws, code, why):
            self.connected = False
            self.log(f"stream closed ({code} {str(why)[:80]})")

        self.ws = self.websocket.WebSocketApp(uri, on_open=on_open, on_message=on_msg, on_error=on_err, on_close=on_close)
        self.ws.run_forever(ping_interval=20, ping_timeout=10, sslopt={"cert_reqs": ssl.CERT_REQUIRED})   # TLS verified

    # --- once a second: the files the engine and the page read ---
    def write(self, nse_open):
        with self.lock:
            st = {k: dict(v) for k, v in self.state.items()}
        inv = {v: s for s, v in self.keys.items()}
        rows, strip, quotes = {}, {}, {}
        for key, v in st.items():
            sym = inv.get(key)
            if not sym or not v.get("ltp"):
                continue
            lp, pc = v.get("ltp"), v.get("prev")
            row = {"ltp": lp, "prev": pc, "chg": (lp - pc) if pc else None, "open": v.get("open"), "high": v.get("high"), "low": v.get("low"),
                   "vol": v.get("vol"), "avg": v.get("avg"), "bids": v.get("bids", []), "asks": v.get("asks", []), "tbq": v.get("tbq"), "tsq": v.get("tsq"),
                   "lc": None, "uc": None, "ltt": v.get("ltt"), "age": round(time.time() - v.get("t", 0), 1)}
            if sym in U.INDICES or sym.startswith("MCX "):
                strip[sym] = {"ltp": lp, "prev": pc, "vol": v.get("vol"), "contract": next((s for u, kk, s in self.feed.mcx if kk == key), None)}
            else:
                rows[sym] = row
                quotes[sym] = (lp, pc or 0, v.get("vol") or 0)
        U.write_json({"source": "Upstox stream (Analytics Token, read-only), every second", "fetched": int(time.time() * 1000), "nse_open": nse_open,
                      "stocks": rows, "strip": strip, "missing": [s for s in U.wanted() if s not in self.feed.eq], "stream": True})
        n = strip.get("NIFTY 50") or {}
        msg = f"Upstox stream: {len(quotes)} stocks, NIFTY 50 {n.get('ltp')}, {self.msgs} messages" + (f"; {self.err}" if self.err else "")
        os.makedirs(U.OUT, exist_ok=True)
        lines = [f"LIVE\t{int(time.time() * 1000)}\t{1 if (nse_open and quotes) else 0}\t{msg if nse_open else 'market closed for NSE; ' + msg}"]
        if n.get("ltp"):
            lines.append(f"X\tNIFTY 50\t{n['ltp']}\t{n.get('prev') or 0}")
        for sym, (p, pc, vol) in sorted(quotes.items()):
            lines.append(f"S\t{sym}\t{p}\t{pc}\t{int(vol)}")
        tmp = os.path.join(U.OUT, "quotes.tsv.tmp")
        open(tmp, "w").write("\n".join(lines) + "\n")
        os.replace(tmp, os.path.join(U.OUT, "quotes.tsv"))
        return msg


def run(log=print, fallback_loop=4):
    token = os.environ.get("UPSTOX_ANALYTICS_TOKEN", "").strip()
    if not token:
        raise SystemExit("UPSTOX_ANALYTICS_TOKEN is not set")
    say = lambda m: log(U.now_ist().strftime("%H:%M:%S") + " " + m)
    if not deps_ok():
        say("stream needs the packages websocket-client and upstox-python-sdk (scripts/update.sh installs them); using 4-second quotes")
        return U.run(fallback_loop, False, log=log)
    s = Stream(token, say)
    fails, last_log = 0, 0.0
    while True:
        nse, mcx = U.market_window(U.now_ist())
        if not (nse or mcx):
            U.write_tsv(False, "market closed (NSE 09:15-15:30 IST, Monday to Friday); Upstox stream waiting")
            time.sleep(60)
            continue
        th = threading.Thread(target=lambda: _guard(s, say), daemon=True)
        th.start()
        started = time.time()
        while th.is_alive():
            time.sleep(1)
            nse, mcx = U.market_window(U.now_ist())
            msg = s.write(nse)
            if time.time() - last_log >= (30 if nse else 600):
                say(msg)
                last_log = time.time()
            if s.connected and time.time() - s.last_msg > 60 and time.time() - started > 60 and nse:
                say("no stream message for 60 s: reconnecting")
                try:
                    s.ws.close()
                except Exception:
                    pass
            if not (nse or mcx):
                try:
                    s.ws.close()
                except Exception:
                    pass
        fails = fails + 1 if s.msgs == 0 or time.time() - started < 120 else 0
        if fails >= 3:                              # the stream does not work here: 4-second quotes for 10 minutes
            say("stream failed 3 times" + (f" ({s.err})" if s.err else "") + ": 4-second quotes for 10 minutes, then the stream again")
            end = time.time() + 600
            f = U.Feed(token, say)
            while time.time() < end:
                nse, mcx = U.market_window(U.now_ist())
                try:
                    f.cycle(nse)
                except Exception as e:
                    U.write_tsv(False, f"Upstox: {type(e).__name__}: {str(e)[:200]}")
                time.sleep(fallback_loop if nse else 30)
            fails = 0
        time.sleep(min(60, 5 * max(1, fails)))


def _guard(s, say):
    try:
        s.run_ws()
    except Exception as e:
        s.err = f"{type(e).__name__} {str(e)[:150]}"
        say("stream: " + s.err)
