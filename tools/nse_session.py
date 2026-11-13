"""Shared NSE website session: NSE's JSON API needs a session cookie from a normal page first."""
import http.cookiejar, json, time, urllib.request
UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36"
WARM = "https://www.nseindia.com/static/regulations/listing-compliance"


class Nse:
    def __init__(self, delay=0.4):
        self.op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
        self.warm, self.delay, self.last = 0.0, delay, 0.0

    def get_json(self, url, referer="https://www.nseindia.com/companies-listing/corporate-filings-announcements", tries=2):
        for k in range(tries):
            if time.time() - self.warm > 600 or k > 0:
                try: self.op.open(urllib.request.Request(WARM, headers={"User-Agent": UA}), timeout=30).read()
                except Exception: pass
                self.warm = time.time()
            wait = self.delay - (time.time() - self.last)
            if wait > 0: time.sleep(wait)
            self.last = time.time()
            try:
                req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "application/json", "Referer": referer})
                with self.op.open(req, timeout=40) as r:
                    return json.loads(r.read())
            except Exception:
                if k == tries - 1: raise
        return None
