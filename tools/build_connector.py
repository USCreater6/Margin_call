#!/usr/bin/env python3
"""Builds web/data/connector.json for the terminal's Connector page.

Listings: every NSE equity (EQUITY_L.csv) and every BSE cash-market instrument (BSE daily
bhavcopy), matched by ISIN. Links (ownership only; no supplier or customer data):
  * Wikidata  (has subsidiary P355, parent organization P749, owned by P127, owner of P1830)
  * GLEIF     (direct parent / direct children), for companies whose LEI is in Wikidata
  * tools/connector_annual_reports.csv  (manually sourced annual-report lists)
  * NSE bulk related-party file (Regulation 23(9) filings, all NSE companies): subsidiaries,
    associates, joint ventures, holding/parent, fellow subsidiaries, group entities. People
    (directors, KMP, relatives) are excluded.
Index membership (NIFTY 50 ... NIFTY 500, Midcap 150, Smallcap 250) is attached to each company.
A link shown on one company is also shown in reverse on the other company.
Output: web/data/connector.json (listings) and web/data/links/<ISIN>.json (links per company).
Usage: build_connector.py [--out web/data/connector.json] [--date YYYY-MM-DD]
"""
import argparse, csv, datetime as dt, http.cookiejar, io, json, os, re, sys, time, urllib.error, urllib.parse, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fetch_nse import HEADERS   # noqa: E402
import xlsx_lite   # noqa: E402

NSE_LIST = "https://nsearchives.nseindia.com/content/equities/EQUITY_L.csv"
BSE_CM = "https://www.bseindia.com/download/BhavCopy/Equity/BhavCopy_BSE_CM_0_0_0_{ymd}_F_0000.CSV"
WDQS = "https://query.wikidata.org/sparql"
GLEIF = "https://api.gleif.org/api/v1/lei-records/{lei}/{rel}?page%5Bsize%5D=200"
WD_UA = {"User-Agent": "Margin_call-connector/0.2 (private research; GitHub USCreater6/Margin_call)",
         "Accept": "application/sparql-results+json"}
REL = {"P355": "subsidiary", "P749": "parent", "P127": "owner", "P1830": "owned company"}
REVERSE = {"subsidiary": "parent", "parent": "subsidiary", "owner": "owned company", "owned company": "owner",
           "associate": "investor", "investor": "associate", "joint venture": "joint venture partner",
           "joint venture partner": "joint venture", "fellow subsidiary": "fellow subsidiary",
           "group or related party": "group or related party"}
INDEXES = [("NIFTY 50", "nifty50"), ("NIFTY NEXT 50", "niftynext50"), ("NIFTY 100", "nifty100"), ("NIFTY 200", "nifty200"),
           ("NIFTY 500", "nifty500"), ("NIFTY MIDCAP 150", "niftymidcap150"), ("NIFTY SMALLCAP 250", "niftysmallcap250")]
INDEX_URL = "https://nsearchives.nseindia.com/content/indices/ind_{}list.csv"
RPT_PAGE = "https://www.nseindia.com/static/regulations/listing-compliance"
# Relationship text in the filings is free text (about 6,700 variants). These rules map it to a group;
# the original wording is kept. People are excluded by relationship words AND by requiring an entity-like name.
PERSON_REL = re.compile(r"director|\bkmp\b|key manag|relative|secretary|chief|\bcfo\b|\bceo\b|managing|whole[- ]?time|"
                        r"independent|chairman|chairperson|executive|personnel|individual|spouse|son of|daughter|wife|husband|"
                        r"\bhuf\b|employee|officer|member of", re.I)
ENTITY_NAME = re.compile(r"\b(limited|ltd|llp|private|pvt|inc|corp|corporation|company|co\.|trust|fund|bank|gmbh|s\.?a\.?|llc|plc|"
                         r"b\.?v\.?|pte|holdings?|ventures|industries|foundation|enterprises|society|partners|ag|n\.?v\.?|"
                         r"s\.?p\.?a|sdn|bhd|kk|oy|ab|as|srl|sarl|pty|lp|group|association|authority|government|board)\b", re.I)
def classify(rel):
    r = rel.lower()
    if "fellow" in r or "subsidiary of ultimate" in r or "subsidiary of holding" in r or "subsidiaries of ultimate" in r \
            or "subsidiary of the holding" in r or "subsidiary of a holding" in r or "subsidiary of parent" in r:
        return "fellow subsidiary"
    if "over entity" in r or "over the entity" in r or "over the company" in r or "over the listed" in r:
        return "group or related party"   # an investor with control/influence over the company
    if "joint venture" in r or "joint control" in r or re.search(r"\bjv\b", r):
        return "joint venture"
    if "associate" in r and "holding" not in r and "parent" not in r:
        return "associate"
    if "associate" in r:
        return "group or related party"   # e.g. associate of the holding company
    if re.search(r"holding company|parent|ultimate holding|holding entity", r) and "subsidiar" not in r.replace("holding subsidiary", ""):
        return "parent"
    if "subsidiar" in r or "step down" in r or "step-down" in r or "wos" in r:
        return "subsidiary"
    return "group or related party"
def fetch_rpt(cache_dir):
    """Newest NSE bulk related-party workbook. Returns (rows iterator, label) or (None, reason)."""
    op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
    def g(u, timeout=240):
        with op.open(urllib.request.Request(u, headers=HEADERS), timeout=timeout) as r: return r.read()
    try:
        page = g(RPT_PAGE).decode("utf-8", "replace")
    except Exception as e:
        return None, f"listing-compliance page unreachable: {e}"
    found = re.findall(r'Related Party Transactions as on ([^<]+?)</[^>]+>.{0,600}?href="([^"]+)"', page, re.S)
    if not found:
        return None, "no related-party file link found on the NSE page"
    label, url = found[0][0].replace("&nbsp;", " ").strip(), found[0][1]
    path = os.path.join(cache_dir, re.sub(r"[^A-Za-z0-9]+", "_", label) + ".xlsx")
    if not os.path.exists(path):
        try:
            open(path, "wb").write(g(url))
        except Exception as e:
            return None, f"download failed: {e}"
    return xlsx_lite.rows(path), label


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
    idx_of = {}
    for label, slug in INDEXES:
        t = get(INDEX_URL.format(slug), tries=2)
        if not t: print(f"index list unavailable: {label}"); continue
        for r in csv.DictReader(io.StringIO(t)):
            i = (r.get("ISIN Code") or "").strip()
            if i: idx_of.setdefault(i, []).append(label)
    print(f"index membership loaded for {len(idx_of)} companies")
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
        norm = lambda n: " ".join(n.casefold().replace(".", " ").replace(" limited", " ltd").split())
        key = other_isin or norm(name)
        for row in lst:   # one row per company; relations and sources are merged
            if (row[1] or norm(row[0])) == key:
                if rel not in row[2].split(" / "): row[2] += " / " + rel
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

    # ---- NSE bulk related-party filings ----
    norm_name = lambda n: re.sub(r"[^a-z0-9 ]", "", " ".join(n.casefold().replace("limited", "ltd").replace(".", " ").split()))
    by_name = {}
    for i, c in comp.items(): by_name.setdefault(norm_name(c["n"]), i)
    os.makedirs(".cache/nse", exist_ok=True)
    it, label = fetch_rpt(".cache/nse")
    n_rpt = n_people = n_rows = 0
    rpt_label = None
    if it is None:
        print("NSE related-party file not used:", label)
    else:
        rpt_label = label
        hdr = next(it)
        col = {h.strip(): k for k, h in enumerate(hdr)}
        need = ["SYMB_SYMBOL", "Details of the counterparty_Name", "Relationship of the counterparty with the listed entity or its subsidiary"]
        if any(h not in col for h in need):
            print("NSE related-party file: expected columns missing; file format changed:", [h for h in need if h not in col])
        else:
            cs, cn, cr = (col[h] for h in need)
            seen = set()
            for r in it:
                n_rows += 1
                if len(r) <= max(cs, cn, cr): continue
                sym, name, rel = r[cs].strip(), " ".join(r[cn].split()), " ".join(r[cr].split())
                isin = by_nse.get(sym)
                if not isin or not name or not rel: continue
                if PERSON_REL.search(rel) or not ENTITY_NAME.search(name):
                    n_people += 1; continue
                key = (isin, name.casefold(), rel.casefold())
                if key in seen: continue
                seen.add(key)
                grp = classify(rel)
                other = by_name.get(norm_name(name), "")
                add_both(isin, comp[isin]["n"], other, name, grp, f"NSE RPT filing ({label}): {rel}")
                n_rpt += 1
        print(f"NSE related-party file ({label}): {n_rows} rows, {n_rpt} company links kept, {n_people} people/other rows excluded")

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

    link_dir = os.path.join(os.path.dirname(a.out), "links")
    os.makedirs(link_dir, exist_ok=True)
    for f in os.listdir(link_dir): os.remove(os.path.join(link_dir, f))
    for isin, lst in links.items():
        if lst: json.dump(lst, open(os.path.join(link_dir, isin + ".json"), "w"), separators=(",", ":"))
    out = {"asof": str(dt.date.today()), "bse_file": str(bse_day) if bse_day else None, "rpt": rpt_label,
           "note": "Company relationships from NSE related-party filings, Wikidata, GLEIF and annual-report lists. "
                   "No supplier or customer data. People are excluded.",
           "c": [[isin, c["nse"], c["bse"], c["n"], len(links.get(isin, [])), "|".join(idx_of.get(isin, []))]
                 for isin, c in sorted(comp.items(), key=lambda kv: kv[1]["nse"] or kv[1]["n"])]}
    json.dump(out, open(a.out, "w"), separators=(",", ":"))
    with_links = sum(1 for v in links.values() if v)
    n50 = [i for i, v in idx_of.items() if "NIFTY 50" in v]
    print(f"wrote {a.out}: {len(comp)} companies, {with_links} with at least one link")
    print(f"NIFTY 50 coverage: {sum(1 for i in n50 if links.get(i))} of {len(n50)} have links")
    for i in n50[:50]:
        print(f"   {comp.get(i, {}).get('nse', i):12} {len(links.get(i, [])):4} links")


if __name__ == "__main__":
    main()
