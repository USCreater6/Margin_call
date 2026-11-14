#!/usr/bin/env python3
"""Real starting prices for the terminal, plus daily price history for every NSE equity.

Usage: fetch_latest.py [--out data/latest] [--hist web/data/history] [--days 370]

Writes:
  data/latest/instruments.csv   starting prices = last trading day's official close
  web/data/history/SYMBOL.csv   daily OHLCV for every NSE EQ-series symbol over --days calendar days
  web/data/history/_index.json  date range and symbol count
Sources: NSE archive daily files (CM equity bhavcopy, FO bhavcopy). MCX is not used: its site
returns 403 to automated downloads, so no commodity futures are included.
Splits and bonuses are adjusted only for entries in tools/corporate_actions.csv; any other
opening jump above 40% is written into that symbol's file header as UNADJUSTED.
"""
import argparse, csv, datetime as dt, io, json, os, sys, time, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fetch_nse import UD, LG, fetch, url_for   # noqa: E402

FO = "https://nsearchives.nseindia.com/content/fo/BhavCopy_NSE_FO_0_0_0_{ymd}_F_0000.csv.zip"
WATCH = ["RELIANCE", "HDFCBANK", "TCS", "INFY", "GOLDBEES"]   # NSE cash-market rows in the watchlist
SIM_TICK = "0.05"   # simulation price step; NOT the verified exchange tick size


def read_zip(blob):
    z = zipfile.ZipFile(io.BytesIO(blob))
    return z.read(z.namelist()[0]).decode("utf-8", "replace")


def get_day(d, cache, delay):
    """CM bhavcopy rows for date d, or None if the exchange has no file (holiday)."""
    path = os.path.join(cache, d.strftime("%Y%m%d") + ".zip")
    if os.path.exists(path):
        blob, st = open(path, "rb").read(), 200
    else:
        url, _ = url_for(d)
        blob, st = fetch(url)
        if st == 200:
            open(path, "wb").write(blob); time.sleep(delay)
    if st == 404:
        return None
    if st != 200:
        raise RuntimeError(f"{d}: HTTP {st}")
    _, col = url_for(d)
    rd = csv.DictReader(io.StringIO(read_zip(blob)))
    rd.fieldnames = [f.strip() for f in rd.fieldnames]
    out = {}
    for r in rd:
        if r[col["ser"]].strip() != "EQ":
            continue
        try:
            out[r[col["sym"]].strip()] = (float(r[col["o"]]), float(r[col["h"]]), float(r[col["l"]]), float(r[col["c"]]), int(float(r[col["v"]])))
        except ValueError:
            pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="data/latest"); ap.add_argument("--hist", default="web/data/history")
    ap.add_argument("--days", type=int, default=370); ap.add_argument("--cache", default=".cache/nse")
    ap.add_argument("--delay", type=float, default=0.3)
    ap.add_argument("--no-history", action="store_true", help="only write starting prices (fast)")
    a = ap.parse_args()
    for p in (a.out, a.hist, a.cache): os.makedirs(p, exist_ok=True)

    today = dt.datetime.now(dt.timezone(dt.timedelta(hours=5, minutes=30))).date()
    last, rows_last = None, None
    for k in range(0, 12):                     # latest day that has a file
        d = today - dt.timedelta(days=k)
        try:
            r = get_day(d, a.cache, a.delay)
        except RuntimeError as e:
            print("unreachable:", e); continue
        if r:
            last, rows_last = d, r; break
    if not last:
        print("NO NSE FILE FOUND in the last 12 days: archive unreachable from this machine."); sys.exit(3)
    print("last trading day with an NSE file:", last)

    lines = [f"# Replay day = NSE trading day {last}: official open, high, low, close and previous close from the NSE daily files.",
             f"# The order of high and low inside the day and every move between them are SIMULATED (daily files carry no intraday path).",
             f"# tick column is a simulation step ({SIM_TICK}), not the verified exchange tick size.",
             "symbol,venue,price,tick,lot,source,date,open,high,low,close,prev_close,volume_units,volume_check"]

    def vol_units(vol, turnover, close, lot):
        """Traded quantity in units, verified against turnover: turnover / (volume x close) must be ~1
        (volume already in units) or ~lot (volume counted in contracts). Otherwise the unit is not verified: 0."""
        try:
            vol, turnover, close = float(vol), float(turnover), float(close)
        except (TypeError, ValueError):
            return 0, "volume or turnover missing"
        if vol <= 0 or close <= 0 or turnover <= 0:
            return 0, "volume or turnover missing"
        r = turnover / (vol * close)
        if 0.8 < r < 1.25:
            return int(vol), f"units (turnover/volume/close = {r:.3f})"
        if lot > 1 and 0.8 < r / lot < 1.25:
            return int(vol * lot), f"contracts x lot {lot} (turnover/volume/close = {r:.1f})"
        return 0, f"unit not verified (turnover/volume/close = {r:.3f})"

    blob, st = fetch(FO.format(ymd=last.strftime("%Y%m%d")))
    if st == 200:
        futs = [r for r in csv.DictReader(io.StringIO(read_zip(blob)))
                if r["TckrSymb"].strip() == "NIFTY" and r["FinInstrmTp"].strip() == "IDF"]
        futs.sort(key=lambda r: r["XpryDt"])
        if futs:
            f = futs[0]   # nearest expiry
            lot = int(f["NewBrdLotQty"])
            vu, vnote = vol_units(f.get("TtlTradgVol"), f.get("TtlTrfVal"), f.get("ClsPric"), lot)
            print(f"NIFTY-FUT volume: {vnote}")
            lines.append(",".join([
                "NIFTY-FUT", "NSE F&O", f"{float(f['ClsPric']):.2f}", SIM_TICK, str(lot),
                f"NSE close {last} {f['FinInstrmNm'].strip()}", str(last),
                f"{float(f['OpnPric']):.2f}", f"{float(f['HghPric']):.2f}", f"{float(f['LwPric']):.2f}", f"{float(f['ClsPric']):.2f}",
                f"{float(f['PrvsClsgPric']):.2f}", str(vu), vnote.replace(",", ";")]))
    else:
        print(f"F&O file not available (HTTP {st}); NIFTY-FUT omitted")
    _, col = url_for(last)
    raw = {}
    blob = open(os.path.join(a.cache, last.strftime("%Y%m%d") + ".zip"), "rb").read()
    rd = csv.DictReader(io.StringIO(read_zip(blob)))
    rd.fieldnames = [x.strip() for x in rd.fieldnames]
    for r in rd:
        if r[col["ser"]].strip() == "EQ" and r[col["sym"]].strip() in WATCH:
            raw[r[col["sym"]].strip()] = r
    for s in WATCH:
        r = raw.get(s)
        if not r:
            print(f"{s}: not in EQ series on {last}; omitted"); continue
        vu, vnote = vol_units(r.get(col["v"]), r.get("TtlTrfVal"), r.get(col["c"]), 1)
        print(f"{s} volume: {vnote}")
        lines.append(",".join([
            s, "NSE", f"{float(r[col['c']]):.2f}", SIM_TICK, "1", f"NSE close {last}", str(last),
            f"{float(r[col['o']]):.2f}", f"{float(r[col['h']]):.2f}", f"{float(r[col['l']]):.2f}", f"{float(r[col['c']]):.2f}",
            f"{float(r[col['pc']]):.2f}", str(vu), vnote.replace(",", ";")]))
    open(os.path.join(a.out, "instruments.csv"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    if a.no_history:
        return

    # daily history for all symbols
    acts = {}
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "corporate_actions.csv")) as f:
        for r in csv.DictReader(l for l in f if not l.startswith("#")):
            acts.setdefault(r["symbol"].strip(), []).append((dt.date.fromisoformat(r["ex_date"].strip()), float(r["factor"])))
    hist = {}
    d, n_days, fails = last - dt.timedelta(days=a.days), 0, 0
    while d <= last:
        try:
            r = rows_last if d == last else get_day(d, a.cache, a.delay)
        except RuntimeError as e:
            print("skipped", e); fails += 1; r = None
        if r:
            n_days += 1
            for s, v in r.items(): hist.setdefault(s, []).append((d, v))
        d += dt.timedelta(days=1)
    for s, rs in hist.items():
        out, notes = [], []
        for d0, (o, h, l, c, v) in rs:
            f = 1.0
            for ex, fac in acts.get(s, []):
                if d0 < ex: f *= fac
            out.append((d0, o / f, h / f, l / f, c / f, int(v * f)))
        for i in range(1, len(out)):
            if out[i - 1][4] > 0 and abs(out[i][1] / out[i - 1][4] - 1) > 0.40:
                notes.append(f"# UNADJUSTED opening jump on {out[i][0]}: {out[i - 1][4]:.2f} to {out[i][1]:.2f} (possible split or bonus)")
        with open(os.path.join(a.hist, s + ".csv"), "w") as f:
            f.write(f"# NSE daily bhavcopy, EQ series, {out[0][0]}..{out[-1][0]}. Splits/bonuses adjusted only per tools/corporate_actions.csv.\n")
            for nline in notes: f.write(nline + "\n")
            f.write("date,open,high,low,close,volume\n")
            for d0, o, h, l, c, v in out:
                f.write(f"{d0.strftime('%Y%m%d')},{o:.2f},{max(h, o, c):.2f},{min(l, o, c):.2f},{c:.2f},{v}\n")
    json.dump({"asof": str(last), "trading_days": n_days, "failed_days": fails, "symbols": len(hist)},
              open(os.path.join(a.hist, "_index.json"), "w"))
    print(f"history: {len(hist)} symbols, {n_days} trading days, {fails} failed days")


if __name__ == "__main__":
    main()
