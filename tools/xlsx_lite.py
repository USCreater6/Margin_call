"""Minimal .xlsx reader (standard library only): yields rows of the first worksheet as lists of strings."""
import re, zipfile
import xml.etree.ElementTree as ET
NS = {"m": "http://schemas.openxmlformats.org/spreadsheetml/2006/main"}

def _col(ref):
    n = 0
    for ch in re.match(r"[A-Z]+", ref).group(0):
        n = n * 26 + ord(ch) - 64
    return n - 1

def rows(path, sheet=None):
    z = zipfile.ZipFile(path)
    shared = []
    if "xl/sharedStrings.xml" in z.namelist():
        for si in ET.fromstring(z.read("xl/sharedStrings.xml")).findall("m:si", NS):
            shared.append("".join(t.text or "" for t in si.iter("{%s}t" % NS["m"])))
    names = sorted(n for n in z.namelist() if re.match(r"xl/worksheets/sheet\d+\.xml$", n))
    name = sheet or names[0]
    with z.open(name) as f:
        for _, el in ET.iterparse(f):
            if el.tag != "{%s}row" % NS["m"]:
                continue
            out = []
            for c in el.findall("m:c", NS):
                i = _col(c.get("r"))
                v = c.find("m:v", NS)
                if c.get("t") == "s" and v is not None:
                    val = shared[int(v.text)]
                elif c.get("t") == "inlineStr":
                    val = "".join(t.text or "" for t in c.iter("{%s}t" % NS["m"]))
                else:
                    val = v.text if v is not None else ""
                while len(out) < i: out.append("")
                out.append(val or "")
            yield out
            el.clear()

def sheets(path):
    z = zipfile.ZipFile(path)
    return sorted(n for n in z.namelist() if re.match(r"xl/worksheets/sheet\d+\.xml$", n))
