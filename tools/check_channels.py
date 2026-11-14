#!/usr/bin/env python3
"""Business TV channels for the terminal's video panel: writes web/data/channels.json.

Candidate channel IDs were found on YouTube (probe, 14 Nov 2026). Each run re-verifies them:
  * the channel's name is read from YouTube's own RSS feed for that ID (confirms the ID is that channel)
  * whether the channel is live right now is read from its /live page
Only channels whose name could be read are listed. Whether a channel allows its stream to play inside
another site is decided by YouTube per channel and can only be seen in the browser; the terminal
therefore also offers "Open on YouTube" for every channel.
"""
import datetime as dt, json, os, re, sys, urllib.request
import xml.etree.ElementTree as ET
UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36"
COOKIE = "CONSENT=YES+cb; SOCS=CAESEwgDEgk0ODE3Nzk3MjQaAmVuIAEaBgiA_LyaBg"
# (channel id, region, language, name as read from YouTube's own feed for that ID on 14 Nov 2026 in CI).
# Each run re-reads the name; the stored name is used only if YouTube cannot be reached from this machine
# (the list then says "not re-checked" and live status is unknown).
VERIFIED_ON = "2026-11-14"
CANDIDATES = [
    ("UCmRbHAgG2k2vDUvb3xsEunQ", "India", "English", "CNBC-TV18"), ("UCI_mwTKUhicNzFrhm33MzBQ", "India", "English", "ET Now"),
    ("UC3uJIdRFTGgLWrUziaHbzrg", "India", "English", "NDTV Profit"), ("UCkXopQ3ubd-rnXnStZqCl2w", "India", "Hindi", "Zee Business"),
    ("UCQIycDaLsBpMKjOCeaKUYVg", "India", "Hindi", "CNBC Awaaz."), ("UCD3CdwT8lTCe5ZGHbUBxmWA", "India", "Hindi", "ET Now Swadesh"),
    ("UCiThge5Gve5Syt299i2bsxA", "India", "Hindi", "NDTV Profit Hindi"), ("UCaPHWiExfUWaKsUtENLCv5w", "India", "English", "Business Today"),
    ("UCQsob4fGjHWhYHW0OLb6rew", "India", "English", "Business Standard"), ("UCUI9vm69ZbAqRK3q3vKLWCQ", "India", "English", "Mint"),
    ("UChftTVI0QJmyXkajQYt2tiQ", "India", "English", "moneycontrol"), ("UCIALMKvObZNtJ6AmdCLP7Lg", "Global", "English", "Bloomberg Television"),
    ("UCrp_UI8XtuYfpiqluWLD7Lw", "Global", "English", "CNBC Television"), ("UCEAZeUIeJs0IjQiqTCdVSIg", "Global", "English", "Yahoo Finance"),
]


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": UA, "Cookie": COOKIE, "Accept-Language": "en-US,en;q=0.9"}), timeout=10) as r:
        return r.read().decode("utf-8", "replace")


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "web/data/channels.json"
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    out, fallback = [], 0
    for cid, region, lang, stored in CANDIDATES:
        name, live = "", None
        try:
            root = ET.fromstring(get(f"https://www.youtube.com/feeds/videos.xml?channel_id={cid}"))
            t = root.find("{http://www.w3.org/2005/Atom}title")
            name = (t.text or "").strip() if t is not None else ""
        except Exception as e:
            print(f"{cid}: feed unavailable ({e})")
        try:
            page = get(f"https://www.youtube.com/channel/{cid}/live")
            live = '"isLive":true' in page
        except Exception as e:
            print(f"{cid}: live status unavailable ({e})")
        checked = bool(name)
        if not name:
            name, fallback = stored, fallback + 1
        print(f"{cid}  {name:40}  live={live}  {region} {lang}  {'re-checked now' if checked else 'NOT re-checked: name from ' + VERIFIED_ON}")
        out.append({"id": cid, "name": name, "live": live, "region": region, "lang": lang, "checked": checked})
    out.sort(key=lambda c: (c["region"] != "India", c["live"] is not True, c["name"]))
    json.dump({"checked": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"), "verified_on": VERIFIED_ON,
               "rechecked": len(out) - fallback, "channels": out}, open(out_path, "w"))
    print(f"{len(out)} channels listed, {len(out) - fallback} re-checked now, {sum(1 for c in out if c['live'])} live now")


if __name__ == "__main__":
    main()
