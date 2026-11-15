#!/usr/bin/env python3
"""NIFTY index options for the terminal, from NSE's official F&O daily file (UDiFF bhavcopy). SIMULATION input only.

Usage: fetch_options.py [--out data/latest] [--date YYYYMMDD] [--strikes 10]

Writes data/latest/options.csv: the nearest NIFTY options expiry, the `--strikes` strikes on each side of the
NIFTY futures close, calls and puts, with the day's official open, high, low, close, previous close, traded
contracts, open interest and lot size. Prints the instrument types and columns the file really has, so nothing
about the file layout is assumed silently.
"""
import argparse, csv, datetime as dt, io, os, sys, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fetch_nse import fetch

FO = "https://nsearchives.nseindia.com/content/fo/BhavCopy_NSE_FO_0_0_0_{ymd}_F_0000.csv.zip"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="data/latest"); ap.add_argument("--date", default="")
    ap.add_argument("--strikes", type=int, default=10)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    today = dt.datetime.now(dt.timezone(dt.timedelta(hours=5, minutes=30))).date()
    days = [dt.datetime.strptime(a.date, "%Y%m%d").date()] if a.date else [today - dt.timedelta(days=k) for k in range(12)]
    blob, day = None, None
    for d in days:
        b, st = fetch(FO.format(ymd=d.strftime("%Y%m%d")))
        print(f"{d}: HTTP {st}")
        if st == 200:
            blob, day = b, d; break
    if not blob:
        print("NO F&O FILE FOUND"); sys.exit(3)
    z = zipfile.ZipFile(io.BytesIO(blob))
    rows = list(csv.DictReader(io.StringIO(z.read(z.namelist()[0]).decode("utf-8", "replace"))))
    rows = [{k.strip(): (v or "").strip() for k, v in r.items()} for r in rows]
    print("columns:", list(rows[0].keys()))
    types = sorted({r["FinInstrmTp"] for r in rows})
    print("instrument types in the file:", types)
    nifty = [r for r in rows if r["TckrSymb"] == "NIFTY"]
    print("NIFTY rows by type:", {t: sum(r["FinInstrmTp"] == t for r in nifty) for t in types})
    futs = sorted([r for r in nifty if r["FinInstrmTp"] == "IDF"], key=lambda r: r["XpryDt"])
    opts = [r for r in nifty if r.get("OptnTp") in ("CE", "PE") and r.get("StrkPric")]
    if not futs or not opts:
        print("NIFTY futures or options not found in the file"); sys.exit(4)
    ref = float(futs[0]["ClsPric"])
    exp = sorted({r["XpryDt"] for r in opts})[0]
    near = [r for r in opts if r["XpryDt"] == exp]
    strikes = sorted({float(r["StrkPric"]) for r in near})
    atm = min(range(len(strikes)), key=lambda k: abs(strikes[k] - ref))
    keep = set(strikes[max(0, atm - a.strikes): atm + a.strikes + 1])
    print("NIFTY futures:", [(r["XpryDt"], r["ClsPric"], r.get("UndrlygPric")) for r in futs])
    print(f"day {day}; nearest expiry {exp}; {len(strikes)} strikes listed; futures close {ref}; keeping {len(keep)} strikes around {strikes[atm]}")
    out = ["# NSE F&O daily file " + str(day) + ": official open, high, low, close, previous close, traded volume and open interest.",
           "# Intraday moves are not in the file. Option prices are premiums per unit; lot = units per contract (NewBrdLotQty).",
           "symbol,expiry,strike,type,open,high,low,close,prev_close,volume,turnover,open_interest,lot,fin_instr_name,underlying_close,settle"]
    for r in sorted(near, key=lambda r: (float(r["StrkPric"]), r["OptnTp"])):
        if float(r["StrkPric"]) not in keep:
            continue
        k = float(r["StrkPric"])
        sym = f"NIFTY {exp} {k:g} {r['OptnTp']}"
        out.append(",".join([sym, exp, f"{k:g}", r["OptnTp"], r["OpnPric"], r["HghPric"], r["LwPric"], r["ClsPric"], r["PrvsClsgPric"],
                             r.get("TtlTradgVol", ""), r.get("TtlTrfVal", ""), r.get("OpnIntrst", ""), r.get("NewBrdLotQty", ""),
                             r.get("FinInstrmNm", "").replace(",", " "), r.get("UndrlygPric", ""), r.get("SttlmPric", "")]))
    open(os.path.join(a.out, "options.csv"), "w").write("\n".join(out) + "\n")
    print("\n".join(out[:8])); print(f"... {len(out) - 3} option rows written to {a.out}/options.csv")


if __name__ == "__main__":
    main()
