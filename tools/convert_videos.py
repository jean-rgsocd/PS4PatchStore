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
import concurrent.futures as cf


def conv(u):
    name = urllib.parse.unquote(u.rsplit("/", 1)[-1])[:-4] + ".jkv"
    try:
        # O ffmpeg le o link direto e so le o trecho que vira .jkv (nao baixa o MP4 inteiro quando o servidor permite).
        subprocess.run([sys.executable, os.path.join(here, "mp4_to_jkv.py"), u, os.path.join(outdir, name)], check=True)
        return 1
    except Exception as e:  # noqa: BLE001
        print("AVISO: nao converti %s (%s)" % (u, e), flush=True)
        return 0


with cf.ThreadPoolExecutor(max_workers=3) as ex:
    res = list(ex.map(conv, urls))
print("videos convertidos: %d, falhas: %d" % (sum(res), len(res) - sum(res)), flush=True)
