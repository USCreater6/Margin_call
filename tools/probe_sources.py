#!/usr/bin/env python3
"""Probe 7: XBRL contexts of a results filing; results API with a date range. Diagnostic only."""
import datetime as dt, json, os, sys, urllib.request
import xml.etree.ElementTree as ET
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nse_session import Nse, UA
nse = Nse()
today = dt.date.today()
for q in ["", f"&from_date={(today - dt.timedelta(days=730)):%d-%m-%Y}&to_date={today:%d-%m-%Y}"]:
    try:
        r = nse.get_json("https://www.nseindia.com/api/corporates-financial-results?index=equities&symbol=RELIANCE&period=Quarterly" + q,
                         "https://www.nseindia.com/companies-listing/corporate-filings-financial-results") or []
        tos = sorted({x.get("toDate") for x in r}, key=lambda s: dt.datetime.strptime(s, "%d-%b-%Y"))
        print(f"results{' (date range)' if q else ''}: {len(r)} rows, period ends {tos[:2]} ... {tos[-3:]}")
        print("  fields:", sorted(r[0].keys()) if r else None)
        print("  cumulative values:", sorted({x.get('cumulative') for x in r}))
    except Exception as e:
        print("results error", e)
url = "https://nsearchives.nseindia.com/corporate/xbrl/INDAS_117298_1348254_16012025082021.xml"
for row in (r or []):
    if row.get("toDate") == "31-Dec-2024" and row.get("consolidated") == "Consolidated":
        url = row["xbrl"]; print("consolidated Q3 row:", {k: row.get(k) for k in ("fromDate", "toDate", "cumulative", "relatingTo", "xbrl")}); break
with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": UA}), timeout=60) as f:
    root = ET.fromstring(f.read())
ctx = {}
for c in root.iter():
    if not c.tag.endswith("}context"): continue
    sd = next((x.text for x in c.iter() if x.tag.endswith("}startDate")), None)
    ed = next((x.text for x in c.iter() if x.tag.endswith("}endDate")), None)
    inst = next((x.text for x in c.iter() if x.tag.endswith("}instant")), None)
    dim = [x.text for x in c.iter() if x.tag.endswith("}explicitMember")]
    ctx[c.get("id")] = (sd, ed, inst, dim)
print("contexts:", len(ctx))
for k, v in list(ctx.items())[:12]: print("  ", k, v)
for el in root.iter():
    n = el.tag.split("}")[-1]
    if n in ("RevenueFromOperations", "ProfitLossForPeriod", "DateOfStartOfReportingPeriod", "DateOfEndOfReportingPeriod", "TypeOfReportingPeriod", "WhetherResultsAreConsolidatedOrStandalone"):
        print(f"  {n:45} ctx={el.get('contextRef'):12} {ctx.get(el.get('contextRef'))} = {(el.text or '')[:40]}")
