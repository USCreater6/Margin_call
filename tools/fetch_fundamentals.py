#!/usr/bin/env python3
"""Fundamentals for the connector's company panel: writes web/data/fund/<SYMBOL>.json.

All from NSE (no key):
  * corporate actions  (dividends, bonuses, splits, rights...)        /api/corporates-corporateActions
  * shareholding pattern (promoter & promoter group %, public %)      /api/corporate-share-holdings-master
  * financial results filings (period, consolidated?, audited?, XBRL) /api/corporates-financial-results
  * key figures from the latest quarterly XBRL filing (consolidated if available), under their official
    XBRL element names, for the filing's own reporting period only. Nothing is renamed or estimated.
Order: NIFTY 50 first, then NIFTY 500, so the most-used companies are ready first.
Usage: fetch_fundamentals.py [--out web/data/fund] [--index nifty500] [--max N] [--symbols A,B]
"""
import argparse, csv, datetime as dt, io, json, os, re, sys, time, urllib.parse, urllib.request
import xml.etree.ElementTree as ET
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nse_session import Nse, UA   # noqa: E402

API = "https://www.nseindia.com/api/"
KEY_TAGS = ["RevenueFromOperations", "OtherIncome", "Income", "Expenses", "FinanceCosts",
            "DepreciationDepletionAndAmortisationExpense", "ProfitBeforeTax", "TaxExpense", "ProfitLossForPeriod",
            "ProfitOrLossAttributableToOwnersOfParent", "BasicEarningsLossPerShareFromContinuingOperations",
            "DilutedEarningsLossPerShareFromContinuingOperations", "PaidUpValueOfEquityShareCapital",
            "InterestEarned", "OperatingProfitBeforeProvisionAndContingencies", "ProvisionsOtherThanTaxAndContingencies"]


def index_members(slug):
    with urllib.request.urlopen(urllib.request.Request(f"https://nsearchives.nseindia.com/content/indices/ind_{slug}list.csv", headers={"User-Agent": UA}), timeout=30) as r:
        return [x["Symbol"].strip() for x in csv.DictReader(io.StringIO(r.read().decode("utf-8", "replace")))]


def d(s, fmt="%d-%b-%Y"):
    try: return dt.datetime.strptime((s or "").strip(), fmt).date()
    except ValueError: return None


def xbrl_figures(url, start, end):
    """Numeric facts for the context covering exactly [start, end] with no dimensions."""
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": UA}), timeout=60) as r:
        root = ET.fromstring(r.read())
    ctx = {}
    for c in root.iter():
        if not c.tag.endswith("}context"): continue
        if any(x.tag.endswith(("}segment", "}scenario")) for x in c.iter()): continue
        sd = next((x.text for x in c.iter() if x.tag.endswith("}startDate")), None)
        ed = next((x.text for x in c.iter() if x.tag.endswith("}endDate")), None)
        if sd and ed and sd.strip() == start.isoformat() and ed.strip() == end.isoformat():
            ctx[c.get("id")] = True
    out = {}
    for el in root.iter():
        if el.get("contextRef") in ctx and el.get("unitRef"):
            name = el.tag.split("}")[-1]
            if name in KEY_TAGS and el.text:
                try: out[name] = {"v": float(el.text), "unit": el.get("unitRef")}
                except ValueError: pass
    return out


def one(nse, sym):
    q = urllib.parse.quote(sym)
    res = {"symbol": sym, "asof": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"), "errors": []}
    try:
        acts = nse.get_json(API + f"corporates-corporateActions?index=equities&symbol={q}", "https://www.nseindia.com/companies-listing/corporate-filings-actions") or []
        res["actions"] = [{"ex": x.get("exDate"), "subject": x.get("subject"), "record": x.get("recDate")} for x in acts][:40]
    except Exception as e: res["errors"].append(f"corporate actions: {e}")
    try:
        sh = nse.get_json(API + f"corporate-share-holdings-master?index=equities&symbol={q}", "https://www.nseindia.com/companies-listing/corporate-filings-shareholding-pattern") or []
        res["shareholding"] = [{"date": x.get("date"), "promoter": x.get("pr_and_prgrp"), "public": x.get("public_val"),
                                "employee_trusts": x.get("employeeTrusts"), "filed": x.get("broadcastDate")} for x in sh][:12]
    except Exception as e: res["errors"].append(f"shareholding: {e}")
    try:
        fr = nse.get_json(API + f"corporates-financial-results?index=equities&symbol={q}&period=Quarterly", "https://www.nseindia.com/companies-listing/corporate-filings-financial-results") or []
        rows = [{"from": x.get("fromDate"), "to": x.get("toDate"), "period": x.get("relatingTo"), "basis": x.get("consolidated"),
                 "audited": x.get("audited"), "filed": x.get("filingDate"), "xbrl": x.get("xbrl")} for x in fr]
        rows.sort(key=lambda r: (d(r["to"]) or dt.date.min, r["basis"] == "Consolidated"), reverse=True)
        res["results"] = rows[:16]
        latest = next((r for r in rows if r["xbrl"] and d(r["from"]) and d(r["to"])), None)
        if latest:
            figs = xbrl_figures(latest["xbrl"], d(latest["from"]), d(latest["to"]))
            res["figures"] = {"period": f"{latest['from']} to {latest['to']}", "basis": latest["basis"], "source": latest["xbrl"], "values": figs}
    except Exception as e: res["errors"].append(f"results: {e}")
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="web/data/fund"); ap.add_argument("--index", default="nifty500")
    ap.add_argument("--max", type=int, default=0); ap.add_argument("--symbols", default="")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if a.symbols:
        syms = [s.strip() for s in a.symbols.split(",") if s.strip()]
    else:
        first = index_members("nifty50")
        syms = first + [s for s in index_members(a.index) if s not in first]
    if a.max: syms = syms[:a.max]
    nse, ok = Nse(delay=0.5), 0
    for k, s in enumerate(syms, 1):
        r = one(nse, s)
        tmp = os.path.join(a.out, s + ".json.tmp")
        json.dump(r, open(tmp, "w"), separators=(",", ":")); os.replace(tmp, os.path.join(a.out, s + ".json"))
        good = bool(r.get("actions") or r.get("shareholding") or r.get("results"))
        ok += good
        if k <= 5 or k % 50 == 0 or r["errors"]:
            fig = r.get("figures", {})
            print(f"[{k}/{len(syms)}] {s}: actions {len(r.get('actions', []))}, shareholding {len(r.get('shareholding', []))}, "
                  f"results {len(r.get('results', []))}, figures {len(fig.get('values', {}))} ({fig.get('basis', '-')}, {fig.get('period', '-')})"
                  + (f" errors: {r['errors']}" if r["errors"] else ""), flush=True)
    print(f"fundamentals: {ok} of {len(syms)} symbols have data")
    sys.exit(0 if ok else 3)


if __name__ == "__main__":
    main()
