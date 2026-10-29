#!/usr/bin/env python3
"""Gera project.gp4 a partir de image0/.
Arquivos fora de eboot.bin e sce_sys/ vao para www/ dentro do app0.
MIN=1 gera um pacote minimo (eboot + sce_sys, sem pic0 e sem site) para testar a instalacao."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_sfo import CONTENT_ID  # noqa: E402

ROOT = "image0"
MIN = os.environ.get("MIN") == "1"

files, dirs = [], set()
for d, _, fs in os.walk(ROOT):
    for f in sorted(fs):
        rel = os.path.relpath(os.path.join(d, f), ROOT).replace(os.sep, "/")
        if rel == "eboot.bin" or rel.startswith("sce_sys/"):
            targ = rel
        else:
            if MIN:
                continue
            targ = "www/" + rel
        if MIN and rel == "sce_sys/pic0.png":
            continue
        files.append((targ, ROOT + "/" + rel))
        parts = targ.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            dirs.add("/".join(parts[:i]))

names = [t for t, _ in files]
for need in ("eboot.bin", "sce_sys/param.sfo", "sce_sys/icon0.png"):
    if need not in names:
        sys.exit("ERRO: falta image0/" + need)
if "sce_sys/about/right.sprx" not in names:
    print("AVISO: image0/sce_sys/about/right.sprx nao encontrado (o exemplo hello_world inclui esse arquivo)")


def tree(prefix, depth):
    out = ""
    for d in sorted(x for x in dirs if x.count("/") == depth and x.startswith(prefix)):
        name = d.split("/")[-1]
        inner = tree(d + "/", depth + 1)
        pad = "  " * (depth + 2)
        if inner:
            out += pad + '<dir targ_name="%s">\n%s%s</dir>\n' % (name, inner, pad)
        else:
            out += pad + '<dir targ_name="%s"/>\n' % name
    return out


file_xml = "".join('    <file targ_path="%s" orig_path="%s"/>\n' % (t, o) for t, o in sorted(files))
xml = '''<?xml version="1.0"?>
<psproject fmt="gp4" version="1000">
  <volume>
    <volume_type>pkg_ps4_app</volume_type>
    <volume_id>PS4VOLUME</volume_id>
    <volume_ts>2026-10-29 12:00:00</volume_ts>
    <package content_id="%s" passcode="00000000000000000000000000000000" storage_type="digital25" app_type="full"/>
    <chunk_info chunk_count="1" scenario_count="1">
      <chunks>
        <chunk id="0" layer_no="0" label="Chunk #0"/>
      </chunks>
      <scenarios default_id="0">
        <scenario id="0" type="sp" initial_chunk_count="1" label="Scenario #0">0</scenario>
      </scenarios>
    </chunk_info>
  </volume>
  <files img_no="0">
%s  </files>
  <rootdir>
%s  </rootdir>
</psproject>
''' % (CONTENT_ID, file_xml, tree("", 0))
with open("project.gp4", "w", encoding="utf-8") as fh:
    fh.write(xml)
print("project.gp4 gerado (%d arquivos, MIN=%s, content_id=%s)" % (len(files), MIN, CONTENT_ID))
