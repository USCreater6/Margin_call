#!/usr/bin/env python3
"""Probe 4: structure of NSE bulk related-party file; market-wide announcements fields. Diagnostic only."""
import collections, http.cookiejar, json, os, sys, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xlsx_lite
UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36"
op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
def get(url, ref="https://www.nseindia.com/", accept="*/*"):
    with op.open(urllib.request.Request(url, headers={"User-Agent": UA, "Accept": accept, "Referer": ref}), timeout=180) as r:
        return r.read()
u = "https://nsearchives.nseindia.com//web/mediaattachment/2026-05/Related_Party_Transactions_as_on_September_30_2025_20260513185352.xlsx"
t0 = time.time(); b = get(u); open("/tmp/rpt.xlsx", "wb").write(b)
print(f"RPT file bytes={len(b)} in {time.time()-t0:.1f}s; sheets={xlsx_lite.sheets('/tmp/rpt.xlsx')}")
for sh in xlsx_lite.sheets("/tmp/rpt.xlsx"):
    n = 0; hdr = None; rel = collections.Counter(); syms = set()
    for i, r in enumerate(xlsx_lite.rows("/tmp/rpt.xlsx", sh)):
        if i < 6: print(f"  {sh} row {i}: {r[:30]}")
        n += 1
    print(f"  {sh}: {n} rows")
get("https://www.nseindia.com/static/regulations/listing-compliance")
a = json.loads(get("https://www.nseindia.com/api/corporate-announcements?index=equities", ref="https://www.nseindia.com/companies-listing/corporate-filings-announcements", accept="application/json"))
print("market-wide announcements:", type(a).__name__, len(a))
for x in (a[:3] if isinstance(a, list) else []): print("  ", json.dumps(x)[:700])
