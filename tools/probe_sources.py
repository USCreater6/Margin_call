#!/usr/bin/env python3
"""Probe 3: NIFTY index lists, NSE bulk related-party file, GDELT adverse news, NSE announcements. Diagnostic only."""
import http.cookiejar, io, json, re, sys, urllib.parse, urllib.request, zipfile
UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36"
cj = http.cookiejar.CookieJar()
op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))
def get(url, ref="https://www.nseindia.com/", accept="*/*", show=300):
    try:
        with op.open(urllib.request.Request(url, headers={"User-Agent": UA, "Accept": accept, "Referer": ref, "Accept-Language": "en-US,en;q=0.9"}), timeout=60) as r:
            b = r.read(); print(f"\n### OK {r.status} {url} bytes={len(b)} type={r.headers.get('Content-Type')}")
            if show: print(b[:show].decode("utf-8", "replace"))
            return b
    except Exception as e:
        print(f"\n### FAIL {url} {e}"); return None
for n in ("nifty50", "niftynext50", "nifty100", "nifty200", "nifty500", "niftymidcap150", "niftysmallcap250", "niftytotalmarket"):
    get(f"https://nsearchives.nseindia.com/content/indices/ind_{n}list.csv", show=120)
get("https://www.nseindia.com/", show=0)   # cookie warm-up
page = get("https://www.nseindia.com/static/regulations/listing-compliance", show=0)
if page:
    t = page.decode("utf-8", "replace")
    for m in re.finditer(r'href="([^"]+)"[^>]*>[^<]*</a>', t):
        pass
    for m in re.finditer(r'Related Party Transactions as on ([^<]+?)</[^>]+>.{0,600}?href="([^"]+)"', t, re.S):
        print("  RPT:", m.group(1).strip(), "->", m.group(2))
    print("  any 'Related Party' mentions:", len(re.findall("Related Party", t)))
    for u in sorted(set(re.findall(r'https?://[^"\']+?(?:\.zip|\.xlsx|\.xls|\.csv)', t)))[:40]: print("  file:", u)
get("https://www.nseindia.com/api/annual-reports?index=equities&symbol=RELIANCE", ref="https://www.nseindia.com/companies-listing/corporate-filings-annual-reports", accept="application/json", show=500)
get("https://www.nseindia.com/api/corporate-announcements?index=equities&symbol=RELIANCE&from_date=01-11-2026&to_date=13-11-2026", ref="https://www.nseindia.com/companies-listing/corporate-filings-announcements", accept="application/json", show=500)
q = '(sourcecountry:IN) (stock OR shares OR sebi OR nse OR bse OR rbi OR fraud OR probe OR default OR downgrade OR penalty) tone<-3'
b = get("https://api.gdeltproject.org/api/v2/doc/doc?" + urllib.parse.urlencode({"query": q, "mode": "ArtList", "format": "json", "maxrecords": "20", "timespan": "1d", "sort": "DateDesc"}), ref="", show=0)
if b:
    try:
        arts = json.loads(b).get("articles", [])
        print("  GDELT articles:", len(arts))
        for a in arts[:8]: print("   ", a.get("seendate"), a.get("domain"), "|", a.get("title", "")[:110], "| keys:", sorted(a.keys()))
    except Exception as e: print("  GDELT parse error:", e, b[:300])
