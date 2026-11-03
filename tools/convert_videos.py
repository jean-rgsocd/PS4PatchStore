#!/usr/bin/env python3
"""Baixa cada MP4 citado em data-video no index.html e converte para .jkv (tools/mp4_to_jkv.py).
Uso: python3 tools/convert_videos.py image0/index.html out/videos
Os .jkv saem com o mesmo nome do MP4; suba-os no R2 na pasta /JKV/ (o catalogo ja aponta para la)."""
import os
import re
import subprocess
import sys
import urllib.parse
import urllib.request

html, outdir = sys.argv[1], sys.argv[2]
os.makedirs(outdir, exist_ok=True)
here = os.path.dirname(os.path.abspath(__file__))
urls = sorted(set(re.findall(r'data-video="([^"]+\.mp4)"', open(html, encoding="utf-8").read(), re.I)))
ok = fail = 0
for u in urls:
    name = urllib.parse.unquote(u.rsplit("/", 1)[-1])[:-4] + ".jkv"
    tmp = os.path.join(outdir, "_tmp.mp4")
    try:
        req = urllib.request.Request(u, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                f.write(b)
        subprocess.run([sys.executable, os.path.join(here, "mp4_to_jkv.py"), tmp, os.path.join(outdir, name)], check=True)
        ok += 1
    except Exception as e:  # noqa: BLE001
        print("AVISO: nao converti %s (%s)" % (u, e))
        fail += 1
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
print("videos convertidos: %d, falhas: %d" % (ok, fail))
