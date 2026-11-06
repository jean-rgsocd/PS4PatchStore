#!/usr/bin/env python3
"""Baixa cada MP4 citado em data-video no index.html e converte para .jkv (tools/mp4_to_jkv.py), VIDEO COMPLETO.
Uso: python3 tools/convert_videos.py image0/index.html out/videos
- Pula os videos que JA existem no R2 (mesmo link, na subpasta JKV/, extensao .jkv): so converte o que for novo.
  Para refazer um video que ja existe, apague o .jkv dele do R2 antes do build.
- Os .jkv novos saem com o mesmo nome do MP4; suba-os no R2 na pasta MP4/JKV/ (o catalogo ja aponta para la)."""
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

ENV = dict(os.environ)
ENV.setdefault("JKV_MAX_SEC", "3600")  # sem o corte de 2 minutos (ate 1 hora)


def jkv_url(u):
    head, _, fname = u.rpartition("/")
    return head + "/JKV/" + fname[:-4] + ".jkv"


def ja_existe(u):
    try:
        req = urllib.request.Request(jkv_url(u), method="HEAD")
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status == 200 and int(r.headers.get("Content-Length") or 0) > 1024
    except Exception:  # noqa: BLE001  (qualquer duvida: converte)
        return False


def conv(u):
    name = urllib.parse.unquote(u.rsplit("/", 1)[-1])[:-4] + ".jkv"
    if ja_existe(u):
        print("PULANDO (ja existe no R2): %s" % name, flush=True)
        return 2
    try:
        # O ffmpeg le o link direto do MP4 (precisa estar no R2) e converte o video inteiro.
        subprocess.run([sys.executable, os.path.join(here, "mp4_to_jkv.py"), u, os.path.join(outdir, name)], check=True, env=ENV)
        return 1
    except Exception as e:  # noqa: BLE001
        print("AVISO: nao converti %s (%s)" % (u, e), flush=True)
        return 0


with cf.ThreadPoolExecutor(max_workers=3) as ex:
    res = list(ex.map(conv, urls))
print("videos novos convertidos: %d, ja existentes (pulados): %d, falhas: %d" % (res.count(1), res.count(2), res.count(0)), flush=True)
