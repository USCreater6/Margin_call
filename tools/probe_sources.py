#!/usr/bin/env python3
"""Probe 6: YouTube business-TV live channels (embeddable?), NSE corporate actions / shareholding / results. Diagnostic."""
import http.cookiejar, json, re, sys, urllib.request
UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36"
op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
def get(url, ref="", accept="*/*", extra=None):
    h = {"User-Agent": UA, "Accept": accept, "Accept-Language": "en-US,en;q=0.9"}
    if ref: h["Referer"] = ref
    h.update(extra or {})
    with op.open(urllib.request.Request(url, headers=h), timeout=40) as r:
        return r.status, r.read().decode("utf-8", "replace")
HANDLES = ["CNBC-TV18", "cnbctv18", "ETNOW", "etnow", "ZeeBusiness", "zeebusiness", "NDTVProfitIndia", "ndtvprofit", "CNBCAwaaz", "cnbcawaaz",
           "ETNowSwadesh", "BloombergTelevision", "markets", "CNBCtelevision", "CNBCInternational", "YahooFinance", "BusinessToday", "moneycontrol",
           "economictimes", "BloombergQuint", "TimesNowNavbharat", "BusinessStandard", "livemint", "NDTVProfitHindi", "ZeeBusinessEnglish"]
cons = {"CONSENT": "YES+cb", "SOCS": "CAESEwgDEgk0ODE3Nzk3MjQaAmVuIAEaBgiA_LyaBg"}
cookie = "; ".join(f"{k}={v}" for k, v in cons.items())
for h in HANDLES:
    try:
        st, t = get(f"https://www.youtube.com/@{h}/live", extra={"Cookie": cookie})
        title = (re.search(r"<title>(.*?)</title>", t) or [None, "?"])[1]
        cid = (re.search(r'"externalId":"(UC[\w-]{22})"', t) or re.search(r'"channelId":"(UC[\w-]{22})"', t) or [None, ""])[1]
        vid = (re.search(r'"videoDetails":\{"videoId":"([\w-]{11})"', t) or [None, ""])[1]
        live = '"isLive":true' in t or '"isLiveContent":true' in t
        emb = ""
        if vid:
            st2, e = get(f"https://www.youtube.com/embed/{vid}", extra={"Cookie": cookie})
            ps = re.search(r'"playabilityStatus":\{"status":"(\w+)"(?:,"reason":"([^"]*)")?', e.replace('\\"', '"'))
            emb = f"embed status={ps.group(1)} reason={ps.group(2)}" if ps else ("embed page has no playability field; " + ("errorScreen" if "errorScreen" in e else "ok?"))
        print(f"@{h:22} {st} channel={cid} live_video={vid} live={live} | {title[:60]} | {emb}")
    except Exception as ex:
        print(f"@{h:22} FAIL {ex}")
# NSE fundamentals feeds
get("https://www.nseindia.com/static/regulations/listing-compliance")
for name, u, ref in [
    ("corporate actions", "https://www.nseindia.com/api/corporates-corporateActions?index=equities&symbol=RELIANCE", "https://www.nseindia.com/companies-listing/corporate-filings-actions"),
    ("shareholding", "https://www.nseindia.com/api/corporate-share-holdings-master?index=equities&symbol=RELIANCE", "https://www.nseindia.com/companies-listing/corporate-filings-shareholding-pattern"),
    ("results", "https://www.nseindia.com/api/corporates-financial-results?index=equities&symbol=RELIANCE&period=Quarterly", "https://www.nseindia.com/companies-listing/corporate-filings-financial-results"),
    ("results-comparison", "https://www.nseindia.com/api/results-comparision?symbol=RELIANCE", "https://www.nseindia.com/companies-listing/corporate-filings-financial-results-comparision")]:
    try:
        st, t = get(u, ref, "application/json")
        j = json.loads(t)
        print(f"\n### {name}: {st} type={type(j).__name__} len={len(j) if hasattr(j,'__len__') else ''}")
        print(json.dumps(j if not isinstance(j, list) else j[:3])[:1500])
    except Exception as ex:
        print(f"\n### {name}: FAIL {ex}")
