#!/usr/bin/env python3
"""Probe 5: related-party file columns and relationship values; announcement keys. Diagnostic only."""
import collections, http.cookiejar, json, os, sys, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xlsx_lite
UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36"
op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
def get(url, ref="https://www.nseindia.com/", accept="*/*"):
    with op.open(urllib.request.Request(url, headers={"User-Agent": UA, "Accept": accept, "Referer": ref}), timeout=180) as r:
        return r.read()
open("/tmp/rpt.xlsx", "wb").write(get("https://nsearchives.nseindia.com//web/mediaattachment/2026-05/Related_Party_Transactions_as_on_September_30_2025_20260513185352.xlsx"))
it = xlsx_lite.rows("/tmp/rpt.xlsx")
hdr = next(it)
for i, h in enumerate(hdr): print(f"  col {i}: {h}")
rel = collections.Counter(); nat = collections.Counter(); rel_rows = []
for r in it:
    r += [""] * (len(hdr) - len(r))
    rel[r[27].strip()] += 1
    if r[4] == "RELIANCE": rel_rows.append(r[23:34])
print("relationship values (top 80):")
for k, v in rel.most_common(80): print(f"  {v:7}  {k[:90]}")
print("distinct relationship values:", len(rel))
print("RELIANCE rows:", len(rel_rows))
for x in rel_rows[:25]: print("  ", x)
get("https://www.nseindia.com/static/regulations/listing-compliance")
a = json.loads(get("https://www.nseindia.com/api/corporate-announcements?index=equities", ref="https://www.nseindia.com/companies-listing/corporate-filings-announcements", accept="application/json"))
print("announcement keys:", sorted(a[0].keys()))
print("desc values:", collections.Counter(x.get("desc") for x in a).most_common(20))
