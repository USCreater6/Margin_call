"""Diagnostic only (overwritten per probe): do NSE's archive master files carry tick sizes?
Tries candidate file names for one date; prints status, header and the rows for a few symbols."""
import gzip, io, sys, os, zipfile, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fetch_nse import fetch

D = sys.argv[2] if len(sys.argv) > 2 else "13112026"   # DDMMYYYY
Y = sys.argv[1] if len(sys.argv) > 1 else "20261113"   # YYYYMMDD
CANDIDATES = [
    f"https://nsearchives.nseindia.com/content/cm/NSE_CM_security_{D}.csv.gz",
    f"https://nsearchives.nseindia.com/content/fo/NSE_FO_contract_{D}.csv.gz",
    f"https://nsearchives.nseindia.com/content/cm/NSE_CM_contract_{D}.csv.gz",
    "https://nsearchives.nseindia.com/content/equities/sec_list.csv",
    f"https://nsearchives.nseindia.com/content/fo/NSE_FO_contract_{D}.csv.zip",
]
WANT = ["RELIANCE", "HDFCBANK", "TCS", "INFY", "GOLDBEES", "NIFTY"]


def text(blob, url):
    if url.endswith(".gz"):
        return gzip.decompress(blob).decode("utf-8", "replace")
    if url.endswith(".zip"):
        z = zipfile.ZipFile(io.BytesIO(blob)); return z.read(z.namelist()[0]).decode("utf-8", "replace")
    return blob.decode("utf-8", "replace")


for url in CANDIDATES:
    blob, st = fetch(url)
    print(f"{st} {url}")
    if st != 200:
        continue
    try:
        t = text(blob, url)
    except Exception as e:
        print("   unreadable:", e); continue
    lines = t.splitlines()
    print("   header:", lines[0][:400])
    shown = 0
    for l in lines[1:]:
        f = l.split(",") if "," in l else l.split("|")
        if any(x.strip() in WANT for x in f[:6]) and shown < 10:
            print("   row:", l[:300]); shown += 1
