#!/usr/bin/env python3
"""Le so o cabecalho de PKGs (por URL ou arquivo local) e preenche o index.html.
Para cada <a ... data-pkg="URL">, define data-content-id, data-size e data-type (ex.: PS4GP).
Uso:  python3 tools/pkg_info.py image0/index.html          (preenche e salva)
      python3 tools/pkg_info.py --show URL_OU_ARQUIVO     (so mostra) """
import re, struct, sys, urllib.request, urllib.parse

TOTAL = {}

def read_range(src, start, length):
    if re.match(r"https?://", src):
        req = urllib.request.Request(src, headers={"Range": "bytes=%d-%d" % (start, start + length - 1),
                                                   "User-Agent": "ps4-patch-store"})
        with urllib.request.urlopen(req, timeout=60) as r:
            data = r.read(length)
            cr = r.headers.get("Content-Range")
            if cr and "/" in cr and cr.split("/")[-1].isdigit():
                TOTAL[src] = int(cr.split("/")[-1])
            if r.status == 200 and start:          # servidor ignorou o Range
                raise RuntimeError("servidor sem suporte a Range")
            return data
    with open(src, "rb") as f:
        f.seek(start); return f.read(length)

def pkg_info(src):
    h = read_range(src, 0, 0x1000)
    if h[:4] != b"\x7fCNT":
        raise RuntimeError("nao e um PKG (cabecalho %s)" % h[:4].hex())
    ec = struct.unpack(">I", h[0x10:0x14])[0]
    eo = struct.unpack(">I", h[0x18:0x1c])[0]
    size = struct.unpack(">Q", h[0x430:0x438])[0]
    content_id = h[0x40:0x64].decode("ascii", "replace")
    table = read_range(src, eo, ec * 32)
    sfo_off = sfo_size = None
    for i in range(ec):
        id_, _fn, _f1, _f2, off, sz = struct.unpack(">IIIIII", table[i*32:i*32+24])
        if id_ == 0x1000: sfo_off, sfo_size = off, sz
    category, title = "", ""
    if sfo_off:
        sfo = read_range(src, sfo_off, sfo_size)
        _m, _v, kt, dt, n = struct.unpack("<4sIIII", sfo[:20])
        for i in range(n):
            ko, _fmt, l, _mx, do = struct.unpack("<HHIII", sfo[20+16*i:36+16*i])
            k = sfo[kt+ko:sfo.index(b"\0", kt+ko)].decode()
            v = sfo[dt+do:dt+do+l].rstrip(b"\0").decode("utf-8", "replace")
            if k == "CATEGORY": category = v
            if k == "TITLE": title = v
    size = TOTAL.get(src, size)          # tamanho real do arquivo no servidor, se conhecido
    return {"content_id": content_id, "size": size, "type": "PS4" + category.upper() if category else "PS4GD", "title": title}

def set_attr(tag, name, value):
    if re.search(r'\b%s="[^"]*"' % name, tag):
        return re.sub(r'\b%s="[^"]*"' % name, '%s="%s"' % (name, value), tag)
    return tag[:-1] + ' %s="%s">' % (name, value)

def fill(path):
    html = open(path, encoding="utf-8").read()
    def repl(m):
        tag = m.group(0); url = re.search(r'data-pkg="([^"]+)"', tag).group(1)
        try:
            i = pkg_info(url)
        except Exception as e:
            print("ERRO", url, "->", e); sys.exit(1)
        print("OK  %s\n    %s  %d bytes  %s  \"%s\"" % (url, i["content_id"], i["size"], i["type"], i["title"]))
        for k, v in (("data-content-id", i["content_id"]), ("data-size", i["size"]), ("data-type", i["type"])):
            tag = set_attr(tag, k, v)
        return tag
    html = re.sub(r'<a\b[^>]*\bdata-pkg="[^"]+"[^>]*>', repl, html)
    open(path, "w", encoding="utf-8").write(html)

if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--show":
        print(pkg_info(sys.argv[2]))
    elif len(sys.argv) == 2:
        fill(sys.argv[1])
    else:
        print(__doc__)
