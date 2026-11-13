#!/usr/bin/env python3
"""Downloads NSE daily bhavcopy files and builds one back-adjusted CSV per symbol.

Usage: fetch_nse.py --start YYYY-MM-DD --end YYYY-MM-DD --out data/nse [--symbols A,B,C]

Source: nsearchives.nseindia.com (official exchange archive). Only the EQ series is used.
Prices are back-adjusted for splits and bonuses using the exchange's own adjusted previous close.
Dividends are NOT adjusted. Adjustments are logged to _adjustments.csv for checking.
Exit codes: 0 ok, 3 archive unreachable or blocked, 4 file format not as expected.
"""
import argparse, csv, datetime as dt, io, os, sys, time, urllib.error, urllib.request, zipfile

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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", required=True); ap.add_argument("--end", required=True)
    ap.add_argument("--out", default="data/nse"); ap.add_argument("--symbols", default=DEFAULT_SYMBOLS)
    ap.add_argument("--cache", default=".cache/nse"); ap.add_argument("--delay", type=float, default=0.35)
    a = ap.parse_args()
    start, end = dt.date.fromisoformat(a.start), dt.date.fromisoformat(a.end)
    want = set(s.strip().upper() for s in a.symbols.split(",") if s.strip())
    os.makedirs(a.out, exist_ok=True); os.makedirs(a.cache, exist_ok=True)
    rows = {s: [] for s in want}
    ok = missing = failed = 0
    d = start
    while d <= end:
        if d.weekday() < 5:
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
    for s, rs in sorted(rows.items()):
        if not rs:
            print(f"{s}: no rows (symbol not found in EQ series)"); continue
        rs.sort(key=lambda r: r[0])
        factor = 1.0
        for i in range(len(rs) - 1, -1, -1):          # walk backwards, scaling everything before each corporate action
            d_, o, h, l, c, pc, v = rs[i]
            rs[i] = [d_, o * factor, h * factor, l * factor, c * factor, pc, int(v / factor) if factor else v]
            if i > 0 and rs[i - 1][4] > 0:
                ratio = pc / rs[i - 1][4]
                if abs(ratio - 1.0) > 0.02:
                    adj_log.append([s, d_.isoformat(), f"{ratio:.6f}"])
                    factor *= ratio
        with open(os.path.join(a.out, s + ".csv"), "w") as f:
            f.write("# NSE daily bhavcopy, EQ series, source nsearchives.nseindia.com. Back-adjusted for splits and bonuses; dividends not adjusted.\n")
            f.write("date,open,high,low,close,volume\n")
            for d_, o, h, l, c, pc, v in rs:
                f.write(f"{d_.strftime('%Y%m%d')},{o:.2f},{max(h, o, c):.2f},{min(l, o, c):.2f},{c:.2f},{v}\n")
        print(f"{s}: {len(rs)} bars {rs[0][0]}..{rs[-1][0]}")
    with open(os.path.join(a.out, "_adjustments.csv"), "w") as f:
        f.write("symbol,ex_date,price_ratio\n")
        for r in adj_log: f.write(",".join(r) + "\n")
    print(f"corporate-action adjustments applied: {len(adj_log)}")
    for r in adj_log: print("  ", *r)


if __name__ == "__main__":
    main()
