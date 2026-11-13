#!/usr/bin/env python3
"""Checks which data sources are reachable and prints their real formats. Diagnostic only."""
import csv, io, json, sys, urllib.parse, urllib.request, zipfile
UA = {"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36",
      "Accept": "*/*", "Referer": "https://www.nseindia.com/all-reports"}
D, DMY = sys.argv[1], sys.argv[2]   # YYYYMMDD, DDMMYYYY

def get(url, data=None, headers=None, show=3):
    h = dict(UA); h.update(headers or {})
    try:
        with urllib.request.urlopen(urllib.request.Request(url, data=data, headers=h), timeout=40) as r:
            b = r.read()
            print(f"\n### OK {r.status} {url}  bytes={len(b)} type={r.headers.get('Content-Type')}")
            if b[:2] == b"PK":
                z = zipfile.ZipFile(io.BytesIO(b)); b = z.read(z.namelist()[0]); print("zip member:", z.namelist()[0])
            t = b.decode("utf-8", "replace")
            for line in t.splitlines()[:show]: print("   ", line[:400])
            return t
    except Exception as e:
        print(f"\n### FAIL {url}  {e}")
        return None

fo = get(f"https://nsearchives.nseindia.com/content/fo/BhavCopy_NSE_FO_0_0_0_{D}_F_0000.csv.zip", show=2)
if fo:
    for r in csv.DictReader(io.StringIO(fo)):
        if r.get("TckrSymb", "").strip() == "NIFTY" and r.get("FinInstrmTp", "").strip() in ("IDF", "FUTIDX"):
            print("   NIFTY FUT row:", {k: v for k, v in r.items() if v})
get(f"https://nsearchives.nseindia.com/content/indices/ind_close_all_{DMY}.csv", show=4)
get("https://nsearchives.nseindia.com/content/equities/EQUITY_L.csv", show=3)
get(f"https://www.bseindia.com/download/BhavCopy/Equity/BhavCopy_BSE_CM_0_0_0_{D}_F_0000.CSV", headers={"Referer": "https://www.bseindia.com/"}, show=3)
get("https://www.mcxindia.com/market-data/bhavcopy", show=0)
get("https://www.mcxindia.com/backpage.aspx/GetDateWiseBhavCopy", data=json.dumps({"Date": D, "InstrumentName": "ALL"}).encode(),
    headers={"Content-Type": "application/json", "Referer": "https://www.mcxindia.com/market-data/bhavcopy"}, show=3)
get("https://api.gleif.org/api/v1/lei-records?filter%5Bentity.legalAddress.country%5D=IN&page%5Bsize%5D=1", show=0)
q = """SELECT (COUNT(DISTINCT ?c) AS ?companies) (COUNT(DISTINCT ?l) AS ?linked) WHERE {
  ?c wdt:P946 ?isin . FILTER(STRSTARTS(?isin, "INE"))
  OPTIONAL { ?c wdt:P355|wdt:P749|wdt:P127|wdt:P1830 ?x . BIND(?c AS ?l) } }"""
get("https://query.wikidata.org/sparql?format=json&query=" + urllib.parse.quote(q),
    headers={"User-Agent": "Margin_call-probe/0.1 (research)", "Accept": "application/sparql-results+json"}, show=20)
