#!/usr/bin/env python3
"""Downloads NSE daily bhavcopy files and builds one back-adjusted CSV per symbol.

Usage: fetch_nse.py --start YYYY-MM-DD --end YYYY-MM-DD --out data/nse [--symbols A,B,C]

Source: nsearchives.nseindia.com (official exchange archive). Only the EQ series is used.
Prices are back-adjusted for the splits and bonuses listed in tools/corporate_actions.csv
(the exchange files are not adjusted). Unexplained large jumps are reported in _gaps.csv.
Dividends are NOT adjusted. Adjustments are logged to _adjustments.csv for checking.
Exit codes: 0 ok, 3 archive unreachable or blocked, 4 file format not as expected.
"""
import argparse, csv, datetime as dt, io, os, re, sys, time, urllib.error, urllib.request, zipfile

UDIFF = "https://nsearchives.nseindia.com/content/cm/BhavCopy_NSE_CM_0_0_0_{ymd}_F_0000.csv.zip"
LEGACY = "https://nsearchives.nseindia.com/content/historical/EQUITIES/{yyyy}/{mon}/cm{dd}{mon}{yyyy}bhav.csv.zip"
UDIFF_FROM = dt.date(2024, 7, 8)
HEADERS = {
    "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36",
    "Accept": "*/*", "Accept-Language": "en-US,en;q=0.9", "Referer": "https://www.nseindia.com/all-reports",
}
DEFAULT_SYMBOLS = ("RELIANCE,HDFCBANK,TCS,INFY,ICICIBANK,ITC,SBIN,LT,HINDUNILVR,BHARTIARTL,NIFTYBEES,GOLDBEES")
UD = dict(sym="TckrSymb", ser="SctySrs", o="OpnPric", h="HghPric", l="LwPric", c="ClsPric", pc="PrvsClsgPric", v="TtlTradgVol")
LG = dict(sym="SYMBOL", ser="SERIES", o="OPEN", h="HIGH", l="LOW", c="CLOSE", pc="PREVCLOSE", v="TOTTRDQTY")


def url_for(d):
    if d >= UDIFF_FROM:
        return UDIFF.format(ymd=d.strftime("%Y%m%d")), UD
    return LEGACY.format(yyyy=d.year, mon=d.strftime("%b").upper(), dd=d.strftime("%d")), LG


def fetch(url):
    """Returns (bytes, status). status: 200, 404, or another HTTP code / -1 on network error."""
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=25) as r:
            return r.read(), 200
    except urllib.error.HTTPError as e:
        return None, e.code
    except Exception as e:  # timeout, reset, DNS
        print(f"  network error: {e}", flush=True)
        return None, -1


BONUS = re.compile(r"bonus\s*(\d+)\s*:\s*(\d+)", re.I)
SPLIT = re.compile(r"(?:split|sub-?division).*?from\s*(?:rs|re)\.?\s*([\d.]+).*?to\s*(?:rs|re)\.?\s*([\d.]+)", re.I)


def auto_actions(rows, start, end):
    """Splits and bonuses from NSE's corporate-actions feed, applied only if the price series confirms them:
    the ex-date open must be within 20% of previous close / factor. Everything is logged."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from nse_session import Nse
    nse, out = Nse(), []
    url = "https://www.nseindia.com/api/corporates-corporateActions?index=equities&symbol={s}&from_date={f}&to_date={t}"
    for sym in sorted(s for s, r in rows.items() if r):
        try:
            data = nse.get_json(url.format(s=urllib.request.quote(sym), f=start.strftime("%d-%m-%Y"), t=end.strftime("%d-%m-%Y")),
                                referer="https://www.nseindia.com/companies-listing/corporate-filings-actions")
        except Exception as e:
            print(f"  corporate actions {sym}: unavailable ({e})"); continue
        by_date = {r[0]: r for r in rows[sym]}
        dates = sorted(by_date)
        for x in data or []:
            subj = (x.get("subject") or "").strip()
            fac = None
            m = BONUS.search(subj)
            if m: fac = (int(m.group(1)) + int(m.group(2))) / int(m.group(2))
            m = SPLIT.search(subj)
            if m and float(m.group(2)) > 0: fac = float(m.group(1)) / float(m.group(2))
            if not fac or fac <= 1: continue
            try: ex = dt.datetime.strptime(x.get("exDate", ""), "%d-%b-%Y").date()
            except ValueError: continue
            if ex not in by_date:
                print(f"  {sym} {ex} '{subj}': ex-date not in the price data, not applied"); continue
            i = dates.index(ex)
            if i == 0: continue
            prev_c, op = by_date[dates[i - 1]][4], by_date[ex][1]
            implied = prev_c / op if op > 0 else 0
            ok = abs(implied / fac - 1) < 0.20
            print(f"  {sym} {ex} '{subj}': factor {fac:g}, price jump implies {implied:.3f} -> {'APPLIED' if ok else 'NOT CONFIRMED, not applied'}")
            if ok: out.append((sym, ex, fac))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", required=True); ap.add_argument("--end", required=True)
    ap.add_argument("--out", default="data/nse"); ap.add_argument("--symbols", default=DEFAULT_SYMBOLS)
    ap.add_argument("--cache", default=".cache/nse"); ap.add_argument("--delay", type=float, default=0.35)
    ap.add_argument("--gap", type=float, default=0.20, help="report open-vs-previous-close moves larger than this")
    ap.add_argument("--index", default="", help="add every member of an NSE index list, e.g. nifty50")
    ap.add_argument("--auto-actions", action="store_true", help="also read splits/bonuses from NSE's corporate-actions feed (applied only when the price data confirms them)")
    a = ap.parse_args()
    start, end = dt.date.fromisoformat(a.start), dt.date.fromisoformat(a.end)
    want = set(s.strip().upper() for s in a.symbols.split(",") if s.strip())
    if a.index:
        blob, st = fetch(f"https://nsearchives.nseindia.com/content/indices/ind_{a.index}list.csv")
        if st != 200:
            print(f"index list {a.index} unavailable (HTTP {st})"); sys.exit(3)
        members = [r["Symbol"].strip() for r in csv.DictReader(io.StringIO(blob.decode("utf-8", "replace")))]
        want |= set(members)
        print(f"index {a.index}: {len(members)} members added; {len(want)} symbols in total")
    os.makedirs(a.out, exist_ok=True); os.makedirs(a.cache, exist_ok=True)
    rows = {s: [] for s in want}
    ok = missing = failed = 0
    d = start
    while d <= end:
        if True:   # weekends included: special sessions exist; non-trading days return 404
            url, col = url_for(d)
            cache = os.path.join(a.cache, d.strftime("%Y%m%d") + ".zip")
            blob, status = (open(cache, "rb").read(), 200) if os.path.exists(cache) else fetch(url)
            if status == 200 and not os.path.exists(cache):
                open(cache, "wb").write(blob); time.sleep(a.delay)
            if status == 200:
                try:
                    z = zipfile.ZipFile(io.BytesIO(blob))
                    text = z.read(z.namelist()[0]).decode("utf-8", "replace")
                except Exception as e:
                    print(f"{d} unreadable zip: {e}"); failed += 1; d += dt.timedelta(days=1); continue
                rd = csv.DictReader(io.StringIO(text))
                rd.fieldnames = [f.strip() for f in rd.fieldnames]
                lost = [v for v in col.values() if v not in rd.fieldnames]
                if lost:
                    print(f"FORMAT ERROR on {d}: columns {lost} not found. File has: {rd.fieldnames}"); sys.exit(4)
                for r in rd:
                    s = r[col["sym"]].strip()
                    if s in want and r[col["ser"]].strip() == "EQ":
                        try:
                            rows[s].append([d, float(r[col["o"]]), float(r[col["h"]]), float(r[col["l"]]), float(r[col["c"]]),
                                            float(r[col["pc"]]), int(float(r[col["v"]]))])
                        except ValueError:
                            pass
                ok += 1
                if ok % 100 == 0: print(f"  {ok} trading days loaded, at {d}", flush=True)
            elif status == 404:
                missing += 1          # exchange holiday
            else:
                failed += 1
                print(f"{d} HTTP {status} {url}", flush=True)
                if ok == 0 and failed >= 5:
                    print("ARCHIVE UNREACHABLE OR BLOCKED from this machine: first 5 requests all failed."); sys.exit(3)
                time.sleep(2)
        d += dt.timedelta(days=1)
    print(f"days loaded {ok}, holidays or no file {missing}, failed {failed}")
    if ok == 0:
        print("No trading days loaded."); sys.exit(3)

    adj_log = []
    gap_log = []
    actions = []
    if a.auto_actions:
        actions += auto_actions(rows, start, end)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "corporate_actions.csv")) as f:
        for row in csv.DictReader(l for l in f if not l.startswith("#")):
            key = (row["symbol"].strip(), dt.date.fromisoformat(row["ex_date"].strip()))
            if not any((x[0], x[1]) == key for x in actions):   # an automatic, price-confirmed entry wins over nothing; manual entries add
                actions.append((key[0], key[1], float(row["factor"])))
    for s, rs in sorted(rows.items()):
        if not rs:
            print(f"{s}: no rows (symbol not found in EQ series)"); continue
        rs.sort(key=lambda r: r[0])
        acts = sorted((d0, f0) for (sy, d0, f0) in actions if sy == s)
        for k, r in enumerate(rs):   # divide prices before each ex-date by the factor; multiply volume
            f = 1.0
            for d0, f0 in acts:
                if r[0] < d0: f *= f0
            if f != 1.0:
                rs[k] = [r[0], r[1] / f, r[2] / f, r[3] / f, r[4] / f, r[5] / f, int(r[6] * f)]
        for d0, f0 in acts:
            if rs[0][0] < d0 <= rs[-1][0]: adj_log.append([s, d0.isoformat(), f"{f0:g}"])
        for i in range(1, len(rs)):
            prev_c = rs[i - 1][4]
            if prev_c > 0 and abs(rs[i][1] / prev_c - 1.0) > a.gap:
                gap_log.append([s, rs[i][0].isoformat(), f"{prev_c:.2f}", f"{rs[i][5]:.2f}", f"{rs[i][1]:.2f}", f"{rs[i][4]:.2f}"])
        with open(os.path.join(a.out, s + ".csv"), "w") as f:
            f.write("# NSE daily bhavcopy, EQ series, source nsearchives.nseindia.com. Back-adjusted for splits and bonuses in tools/corporate_actions.csv; dividends not adjusted.\n")
            f.write("date,open,high,low,close,volume\n")
            for d_, o, h, l, c, pc, v in rs:
                f.write(f"{d_.strftime('%Y%m%d')},{o:.2f},{max(h, o, c):.2f},{min(l, o, c):.2f},{c:.2f},{v}\n")
        print(f"{s}: {len(rs)} bars {rs[0][0]}..{rs[-1][0]}")
    with open(os.path.join(a.out, "_adjustments.csv"), "w") as f:
        f.write("symbol,ex_date,factor\n")
        for r in adj_log: f.write(",".join(r) + "\n")
    with open(os.path.join(a.out, "_gaps.csv"), "w") as f:
        f.write("symbol,date,prev_close_adjusted,prev_close_in_file,open,close\n")
        for r in gap_log: f.write(",".join(r) + "\n")
    print(f"UNEXPLAINED opening jumps (> {a.gap:.0%}) after adjustment: {len(gap_log)}  (each needs a corporate_actions.csv entry or a check)")
    for r in gap_log: print("  GAP", *r)
    print(f"corporate-action adjustments applied: {len(adj_log)}")
    for r in adj_log: print("  ", *r)


if __name__ == "__main__":
    main()
