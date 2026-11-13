#!/usr/bin/env python3
"""Builds web/data/connector.json for the terminal's Connector page.

Listings: every NSE equity (EQUITY_L.csv) and every BSE cash-market instrument (BSE daily
bhavcopy), matched by ISIN. Links (ownership only; no supplier or customer data):
  * Wikidata  (has subsidiary P355, parent organization P749, owned by P127, owner of P1830)
  * GLEIF     (direct parent / direct children), for companies whose LEI is in Wikidata
  * tools/connector_annual_reports.csv  (manually sourced annual-report lists)
A link shown on one company is also shown in reverse on the other company.
Usage: build_connector.py [--out web/data/connector.json] [--date YYYY-MM-DD]
"""
import argparse, csv, datetime as dt, io, json, os, sys, time, urllib.error, urllib.parse, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fetch_nse import HEADERS   # noqa: E402

NSE_LIST = "https://nsearchives.nseindia.com/content/equities/EQUITY_L.csv"
BSE_CM = "https://www.bseindia.com/download/BhavCopy/Equity/BhavCopy_BSE_CM_0_0_0_{ymd}_F_0000.CSV"
WDQS = "https://query.wikidata.org/sparql"
GLEIF = "https://api.gleif.org/api/v1/lei-records/{lei}/{rel}?page%5Bsize%5D=200"
WD_UA = {"User-Agent": "Margin_call-connector/0.2 (private research; GitHub USCreater6/Margin_call)",
         "Accept": "application/sparql-results+json"}
REL = {"P355": "subsidiary", "P749": "parent", "P127": "owner", "P1830": "owned company"}
REVERSE = {"subsidiary": "parent", "parent": "subsidiary", "owner": "owned company", "owned company": "owner",
           "associate": "investor", "investor": "associate", "joint venture": "joint venture partner",
           "joint venture partner": "joint venture"}


def get(url, headers=None, data=None, tries=3):
    h = dict(HEADERS); h.update(headers or {})
    for k in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=h, data=data), timeout=90) as r:
                return r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            if e.code == 404: return None   # e.g. GLEIF: no parent reported
            print(f"  attempt {k + 1} failed for {url[:90]}: {e}")
        except Exception as e:
            print(f"  attempt {k + 1} failed for {url[:90]}: {e}")
            time.sleep(3 * (k + 1))
    return None


def sparql(q):
    t = get(WDQS, WD_UA, urllib.parse.urlencode({"query": q, "format": "json"}).encode())
    return json.loads(t)["results"]["bindings"] if t else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="web/data/connector.json")
    ap.add_argument("--date", default="")
    a = ap.parse_args()
    os.makedirs(os.path.dirname(a.out), exist_ok=True)

    # ---- listings ----
    comp = {}   # isin -> {"n": name, "nse": sym, "bse": code}
    t = get(NSE_LIST)
    if not t: print("NSE equity list unreachable"); sys.exit(3)
    for r in csv.DictReader(io.StringIO(t)):
        r = {k.strip(): (v or "").strip() for k, v in r.items()}
        if r.get("ISIN NUMBER"):
            comp[r["ISIN NUMBER"]] = {"n": r["NAME OF COMPANY"], "nse": r["SYMBOL"], "bse": ""}
    n_nse = len(comp)
    day = dt.date.fromisoformat(a.date) if a.date else dt.datetime.now(dt.timezone(dt.timedelta(hours=5, minutes=30))).date()
    bse_day = None
    for k in range(12):
        d = day - dt.timedelta(days=k)
        t = get(BSE_CM.format(ymd=d.strftime("%Y%m%d")), {"Referer": "https://www.bseindia.com/"}, tries=1)
        if t and t.startswith("TradDt"):
            bse_day = d
            for r in csv.DictReader(io.StringIO(t)):
                if r["FinInstrmTp"].strip() != "STK" or not r["ISIN"].strip(): continue
                c = comp.setdefault(r["ISIN"].strip(), {"n": r["FinInstrmNm"].strip(), "nse": "", "bse": ""})
                c["bse"] = r["FinInstrmId"].strip()
            break
    print(f"listings: {n_nse} NSE, {sum(1 for c in comp.values() if c['bse'])} with BSE code (BSE file {bse_day}), {len(comp)} total")

    by_nse = {c["nse"]: i for i, c in comp.items() if c["nse"]}
    by_bse = {c["bse"]: i for i, c in comp.items() if c["bse"]}
    def by_ticker(tk, exchange):
        tk = tk.strip().upper().replace("NSE:", "").replace("BSE:", "")
        if "National Stock Exchange of India" in exchange: return by_nse.get(tk, "")
        if "Bombay" in exchange or "BSE" in exchange: return by_bse.get(tk, "")
        return ""
    links = {}   # isin -> list of [name, isin_or_empty, relation, source]
    def add(isin, name, other_isin, rel, src):
        if other_isin == isin: return
        lst = links.setdefault(isin, [])
        key = (other_isin or " ".join(name.casefold().replace(".", " ").split()), rel)
        for row in lst:
            if (row[1] or " ".join(row[0].casefold().replace(".", " ").split()), row[2]) == key:
                if src not in row[3].split(" + "): row[3] += " + " + src
                return
        lst.append([name, other_isin or "", rel, src])
    def add_both(a_isin, a_name, b_isin, b_name, rel, src):
        if a_isin in comp: add(a_isin, b_name, b_isin if b_isin in comp else "", rel, src)
        if b_isin and b_isin in comp: add(b_isin, a_name, a_isin if a_isin in comp else "", REVERSE.get(rel, "related"), src)

    # ---- Wikidata ----
    q = """SELECT ?c ?isin ?lei ?p ?t ?tLabel ?tIsin ?tTicker ?tExName WHERE {
      ?c wdt:P946 ?isin . FILTER(STRSTARTS(?isin, "INE"))
      OPTIONAL { ?c wdt:P1278 ?lei }
      OPTIONAL { VALUES ?p { wdt:P355 wdt:P749 wdt:P127 wdt:P1830 } ?c ?p ?t .
                 OPTIONAL { ?t wdt:P946 ?tIsin . FILTER(STRSTARTS(?tIsin, "INE")) }
                 OPTIONAL { ?t p:P414 ?tst . ?tst pq:P249 ?tTicker ; ps:P414 ?tex .
                            ?tex rdfs:label ?tExName . FILTER(LANG(?tExName) = "en") } }
      SERVICE wikibase:label { bd:serviceParam wikibase:language "en". } }"""
    rows = sparql(q)
    # Companies without an ISIN in Wikidata but with an NSE or BSE listing and ticker
    q2 = """SELECT ?c ?ticker ?exLabel ?lei ?p ?t ?tLabel ?tIsin ?tTicker ?tExName WHERE {
      VALUES ?exLabelEn { "National Stock Exchange of India"@en "Bombay Stock Exchange"@en "BSE Limited"@en }
      ?ex rdfs:label ?exLabelEn . ?c p:P414 ?st . ?st ps:P414 ?ex ; pq:P249 ?ticker .
      FILTER NOT EXISTS { ?c wdt:P946 ?anyIsin . FILTER(STRSTARTS(?anyIsin, "INE")) }
      OPTIONAL { ?c wdt:P1278 ?lei }
      OPTIONAL { VALUES ?p { wdt:P355 wdt:P749 wdt:P127 wdt:P1830 } ?c ?p ?t .
                 OPTIONAL { ?t wdt:P946 ?tIsin . FILTER(STRSTARTS(?tIsin, "INE")) }
                 OPTIONAL { ?t p:P414 ?tst . ?tst pq:P249 ?tTicker ; ps:P414 ?tex .
                            ?tex rdfs:label ?tExName . FILTER(LANG(?tExName) = "en") } }
      SERVICE wikibase:label { bd:serviceParam wikibase:language "en". } }"""
    rows2 = sparql(q2)
    if rows2:
        matched = 0
        for b in rows2:
            isin = by_ticker(b["ticker"]["value"], b["exLabel"]["value"])
            if not isin: continue
            b["isin"] = {"value": isin}
            rows = (rows or []) + [b]
            matched += 1
        print(f"Wikidata listings by ticker: {len(rows2)} rows, {matched} matched to an NSE/BSE code")
    leis, wd_links = {}, 0
    if rows is None:
        print("Wikidata unreachable: no Wikidata links")
    else:
        for b in rows:
            isin = b["isin"]["value"]
            if "lei" in b: leis[isin] = b["lei"]["value"]
            if "p" not in b or isin not in comp: continue
            rel = REL[b["p"]["value"].rsplit("/", 1)[1]]
            name = b["tLabel"]["value"]
            if name.startswith("Q") and name[1:].isdigit(): continue   # no English label
            t_isin = b.get("tIsin", {}).get("value", "")
            if not t_isin and "tTicker" in b:
                t_isin = by_ticker(b["tTicker"]["value"], b.get("tExName", {}).get("value", ""))
            add_both(isin, comp[isin]["n"], t_isin, name, rel, "Wikidata")
            wd_links += 1
        print(f"Wikidata: {len(rows)} rows, {wd_links} links, {len(leis)} companies with an LEI")

    # ---- GLEIF (via LEIs from Wikidata) ----
    lei_to_isin = {v: k for k, v in leis.items()}
    g_links = 0
    for isin, lei in sorted(leis.items()):
        if isin not in comp: continue
        for rel_path, rel in (("direct-children", "subsidiary"), ("direct-parent", "parent")):
            t = get(GLEIF.format(lei=lei, rel=rel_path), {"Accept": "application/vnd.api+json"}, tries=2)
            time.sleep(0.2)
            if not t: continue
            data = json.loads(t).get("data") or []
            for x in (data if isinstance(data, list) else [data]):
                ent = x["attributes"]["entity"]
                name = ent["legalName"]["name"]
                if not name.isascii():
                    alts = [o["name"] for o in (ent.get("transliteratedOtherNames") or []) + (ent.get("otherNames") or []) if o.get("name", "").isascii()]
                    if alts: name = alts[0]
                    else: name = name + " (" + (ent.get("legalAddress", {}).get("country") or "?") + ")"
                add_both(isin, comp[isin]["n"], lei_to_isin.get(x["id"], ""), name, rel, "GLEIF")
                g_links += 1
    print(f"GLEIF: {g_links} links")

    # ---- annual reports ----
    here = os.path.dirname(os.path.abspath(__file__))
    n_ar = 0
    with open(os.path.join(here, "connector_annual_reports.csv")) as f:
        for r in csv.DictReader(l for l in f if not l.startswith("#")):
            ci = r["company_isin"].strip()
            if ci in comp:
                add_both(ci, comp[ci]["n"], r["related_isin"].strip(), r["related_name"].strip(), r["relation"].strip(), "Annual report: " + r["source"].strip())
                n_ar += 1
    print(f"annual-report links: {n_ar}")

    out = {"asof": str(dt.date.today()), "bse_file": str(bse_day) if bse_day else None,
           "note": "Ownership links only. Supplier and customer links are not available from free sources.",
           "c": [[isin, c["nse"], c["bse"], c["n"]] for isin, c in sorted(comp.items(), key=lambda kv: kv[1]["nse"] or kv[1]["n"])],
           "l": links}
    json.dump(out, open(a.out, "w"), separators=(",", ":"))
    print(f"wrote {a.out}: {len(comp)} companies, {sum(1 for v in links.values() if v)} with at least one link")


if __name__ == "__main__":
    main()
