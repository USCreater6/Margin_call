#!/usr/bin/env python3
"""Adverse-media feed for the terminal: writes web/data/news.json.

Sources (no API key):
  * NSE corporate announcements (official exchange filings), market-wide and for the watchlist symbols.
  * GDELT DOC 2.0 news search, India business news with negative tone. GDELT rate-limits shared
    machines (HTTP 429); it is tried at most every 15 minutes and skipped quietly when refused.
Tagging:
  * NSE: ADVERSE when the subject or text matches ADVERSE_WORDS (a keyword rule, shown as such).
  * GDELT: ADVERSE when GDELT's own tone score is below -3; the query already asks for tone < -3.
Usage: fetch_news.py [--out web/data/news.json] [--loop SECONDS]
"""
import argparse, datetime as dt, http.cookiejar, json, os, re, sys, time, urllib.parse, urllib.request
UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36"
IST = dt.timezone(dt.timedelta(hours=5, minutes=30))
ANN = "https://www.nseindia.com/api/corporate-announcements?index=equities"
ANN_SYM = ANN + "&symbol={sym}&from_date={f}&to_date={t}"
GDELT = "https://api.gdeltproject.org/api/v2/doc/doc?"
GDELT_Q = ('(sourcecountry:IN) (sebi OR nse OR bse OR rbi OR "stock market" OR shares OR fraud OR probe OR default '
           'OR downgrade OR penalty OR insolvency OR raid) tone<-3')
ADVERSE_WORDS = re.compile(r"penalt|\bfine\b|fined|show cause|litigation|lawsuit|default|delay in payment|downgrad|"
                           r"resign|fraud|search and seizure|\braid|investigat|insolven|\bnclt\b|suspen|pledge|"
                           r"winding up|liquidat|forensic|qualified opinion|non-compliance|disqualif|arrest|"
                           r"order against|tax demand|demand notice|attachment|loss of|shutdown|fire at|accident", re.I)


class Nse:
    def __init__(self):
        self.op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
        self.warm = 0
    def get(self, url):
        if time.time() - self.warm > 600:   # NSE needs a session cookie from a normal page first
            try: self.op.open(urllib.request.Request("https://www.nseindia.com/static/regulations/listing-compliance", headers={"User-Agent": UA}), timeout=30).read()
            except Exception: pass
            self.warm = time.time()
        req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "application/json",
                                                   "Referer": "https://www.nseindia.com/companies-listing/corporate-filings-announcements"})
        with self.op.open(req, timeout=40) as r:
            return json.loads(r.read())


def nse_items(nse, symbols):
    items, errs = [], []
    today = dt.datetime.now(IST).date()
    urls = [ANN] + [ANN_SYM.format(sym=urllib.parse.quote(s), f=(today - dt.timedelta(days=7)).strftime("%d-%m-%Y"), t=today.strftime("%d-%m-%Y")) for s in symbols]
    for u in urls:
        try:
            data = nse.get(u)
        except Exception as e:
            errs.append(f"{u[:90]}: {e}"); continue
        for x in data if isinstance(data, list) else []:
            subj, text = (x.get("desc") or "").strip(), (x.get("attchmntText") or "").strip()
            when = x.get("an_dt") or x.get("exchdisstime") or ""
            try: ts = dt.datetime.strptime(when, "%d-%b-%Y %H:%M:%S").replace(tzinfo=IST).isoformat()
            except ValueError: ts = ""
            m = ADVERSE_WORDS.search(subj + " " + text)
            items.append({"id": "nse:" + str(x.get("seq_id") or x.get("attchmntFile") or subj + when), "t": ts,
                          "src": "NSE filing", "tag": "ADVERSE" if m else "FILING", "why": f"keyword: {m.group(0)}" if m else "",
                          "topic": (x.get("symbol") or "").strip(), "title": f"{(x.get('sm_name') or '').strip()}: {subj}" + (f" — {text[:180]}" if text else ""),
                          "url": x.get("attchmntFile") or ""})
        time.sleep(0.5)
    return items, errs


def gdelt_items():
    q = urllib.parse.urlencode({"query": GDELT_Q, "mode": "ArtList", "format": "json", "maxrecords": "50", "timespan": "1d", "sort": "DateDesc"})
    req = urllib.request.Request(GDELT + q, headers={"User-Agent": "Margin_call-news/0.1 (private research)"})
    with urllib.request.urlopen(req, timeout=40) as r:
        data = json.loads(r.read() or b"{}")
    out = []
    for a in data.get("articles", []):
        sd = a.get("seendate", "")
        try: ts = dt.datetime.strptime(sd, "%Y%m%dT%H%M%SZ").replace(tzinfo=dt.timezone.utc).isoformat()
        except ValueError: ts = ""
        out.append({"id": "gdelt:" + a.get("url", ""), "t": ts, "src": "GDELT · " + a.get("domain", ""), "tag": "ADVERSE",
                    "why": "GDELT tone below -3", "topic": "", "title": a.get("title", ""), "url": a.get("url", "")})
    return out


def run_once(path, nse, symbols, state):
    old = {}
    if os.path.exists(path):
        try: old = {x["id"]: x for x in json.load(open(path)).get("items", [])}
        except Exception: old = {}
    items, errs = nse_items(nse, symbols)
    status = {"nse": "ok" if items else ("error: " + errs[0] if errs else "no items"), "gdelt": state.get("gdelt", "not tried")}
    if time.time() - state.get("gdelt_at", 0) > 900:
        state["gdelt_at"] = time.time()
        try:
            g = gdelt_items(); items += g; state["gdelt"] = f"ok ({len(g)} articles)"
        except Exception as e:
            state["gdelt"] = f"refused: {e}"
        status["gdelt"] = state["gdelt"]
    for x in items: old[x["id"]] = x
    merged = sorted(old.values(), key=lambda x: x["t"], reverse=True)[:300]
    tmp = path + ".tmp"
    json.dump({"updated": dt.datetime.now(IST).isoformat(timespec="seconds"), "status": status, "items": merged}, open(tmp, "w"))
    os.replace(tmp, path)   # atomic: the terminal never reads a half-written file
    print(f"{dt.datetime.now(IST):%H:%M:%S} news: {len(items)} fetched, {len(merged)} kept, "
          f"{sum(1 for x in merged if x['tag'] == 'ADVERSE')} adverse | NSE {status['nse']} | GDELT {status['gdelt']}", flush=True)
    return len(items)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="web/data/news.json"); ap.add_argument("--loop", type=int, default=0)
    ap.add_argument("--instruments", default="data/latest/instruments.csv")
    a = ap.parse_args()
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    symbols = []
    if os.path.exists(a.instruments):
        for line in open(a.instruments):
            if line and not line.startswith(("#", "symbol,")) and ",NSE," in line: symbols.append(line.split(",")[0])
    nse, state = Nse(), {}
    while True:
        n = run_once(a.out, nse, symbols, state)
        if not a.loop:
            sys.exit(0 if n else 3)
        time.sleep(a.loop)


if __name__ == "__main__":
    main()
