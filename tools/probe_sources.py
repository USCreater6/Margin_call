#!/usr/bin/env python3
"""Second probe: commodity alternative to MCX, lot sizes, ownership-data coverage. Diagnostic only."""
import csv, io, json, sys, urllib.parse, urllib.request, zipfile
UA = {"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36",
      "Accept": "*/*", "Referer": "https://www.nseindia.com/all-reports"}
D = sys.argv[1]
def get(url, headers=None):
    h = dict(UA); h.update(headers or {})
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=60) as r:
            b = r.read()
            if b[:2] == b"PK":
                z = zipfile.ZipFile(io.BytesIO(b)); b = z.read(z.namelist()[0])
            print(f"\n### OK {url} bytes={len(b)}"); return b.decode("utf-8", "replace")
    except Exception as e:
        print(f"\n### FAIL {url} {e}"); return None

fo = get(f"https://nsearchives.nseindia.com/content/fo/BhavCopy_NSE_FO_0_0_0_{D}_F_0000.csv.zip")
for r in csv.DictReader(io.StringIO(fo or "")):
    if r["TckrSymb"] in ("NIFTY",) and r["FinInstrmTp"] == "IDF": print("  NIFTY", r["FinInstrmNm"], "close", r["ClsPric"], "settle", r["SttlmPric"], "lot", r["NewBrdLotQty"])
    if r["TckrSymb"] in ("RELIANCE", "HDFCBANK", "TCS") and r["FinInstrmTp"] == "STF": print("  STF", r["FinInstrmNm"], "lot", r["NewBrdLotQty"])
for u in (f"https://nsearchives.nseindia.com/content/com/BhavCopy_NSE_COM_0_0_0_{D}_F_0000.csv.zip",
          f"https://nsearchives.nseindia.com/content/commodity/BhavCopy_NSE_COM_0_0_0_{D}_F_0000.csv.zip"):
    t = get(u)
    if t:
        rows = list(csv.DictReader(io.StringIO(t)))
        print("  columns:", list(rows[0].keys())[:20] if rows else None)
        print("  tickers:", sorted({r.get("TckrSymb", "") for r in rows})[:80])
        for r in rows:
            if any(k in r.get("TckrSymb", "") for k in ("GOLD", "CRUDE")) and r.get("FinInstrmTp", "") in ("IDF", "FUTCOM", "CMF", "FUT", "COF"):
                print("   ", r.get("FinInstrmTp"), r.get("FinInstrmNm"), "close", r.get("ClsPric"), "lot", r.get("NewBrdLotQty"))
eq = get("https://nsearchives.nseindia.com/content/equities/EQUITY_L.csv")
isin = {r["SYMBOL"]: r[" ISIN NUMBER"].strip() for r in csv.DictReader(io.StringIO(eq or ""))}
print("  NSE equities listed:", len(isin), " RELIANCE ISIN:", isin.get("RELIANCE"))
g = get("https://api.gleif.org/api/v1/lei-records?filter%5Bentity.legalAddress.country%5D=IN&page%5Bsize%5D=1")
if g: print("  GLEIF India LEI records total:", json.loads(g)["meta"]["pagination"]["total"])
for s in ("RELIANCE", "HDFCBANK", "TCS"):
    t = get("https://api.gleif.org/api/v1/lei-records?filter%5Bisin%5D=" + isin.get(s, ""))
    if t:
        d = json.loads(t)["data"]
        print(f"  GLEIF by ISIN {s}: {len(d)} record(s)", [x["attributes"]["entity"]["legalName"]["name"] for x in d][:2])
        if d:
            lei = d[0]["id"]
            for rel in ("direct-children", "direct-parent", "ultimate-children"):
                c = get(f"https://api.gleif.org/api/v1/lei-records/{lei}/{rel}?page%5Bsize%5D=200")
                if c:
                    j = json.loads(c); dd = j.get("data") or []
                    dd = dd if isinstance(dd, list) else [dd]
                    print(f"    {rel}: total {j.get('meta',{}).get('pagination',{}).get('total', len(dd))}", [x["attributes"]["entity"]["legalName"]["name"] for x in dd][:6])
q = """SELECT (COUNT(DISTINCT ?c) AS ?onNSE) (COUNT(DISTINCT ?l) AS ?withLinks) WHERE {
  ?ex rdfs:label "National Stock Exchange of India"@en . ?c p:P414/ps:P414 ?ex .
  OPTIONAL { ?c wdt:P355|wdt:P749|wdt:P127|wdt:P1830 ?x . BIND(?c AS ?l) } }"""
print(get("https://query.wikidata.org/sparql?format=json&query=" + urllib.parse.quote(q),
          {"User-Agent": "Margin_call-probe/0.1 (research)", "Accept": "application/sparql-results+json"}))
