
#!/usr/bin/env python3
"""Gera project.gp4 a partir de image0/.

Arquivos fora de eboot.bin, directpayload.bin, sce_sys/ e sce_module/
vao para www/ dentro do app0.

MODE escolhe o conteudo:
  min  = eboot + payload + sce_sys + sce_module (sem pic0 e sem site)
  pic  = min + sce_sys/pic0.png
  www  = min + site em www/ (sem pic0)
  full = tudo (padrao)
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_sfo import CONTENT_ID  # noqa: E402

ROOT = "image0"

MODE = os.environ.get("MODE") or (
    "min" if os.environ.get("MIN") == "1" else "full"
)

if MODE not in ("min", "pic", "www", "full"):
    sys.exit("ERRO: MODE invalido: " + MODE)

WITH_PIC = MODE in ("pic", "full")
WITH_WWW = MODE in ("www", "full")

if not os.path.isdir(ROOT):
    sys.exit("ERRO: diretorio image0 nao encontrado")

files = []
dirs = set()

for d, _, fs in os.walk(ROOT):
    for f in sorted(fs):
        source = os.path.join(d, f)
        rel = os.path.relpath(source, ROOT).replace(os.sep, "/")

        # Arquivos obrigatorios na raiz do app0.
        if (
            rel in ("eboot.bin", "directpayload.bin")
            or rel.startswith("sce_sys/")
            or rel.startswith("sce_module/")
        ):
            targ = rel
        else:
            if not WITH_WWW:
                continue

            targ = "www/" + rel

        # Inclui pic0 somente nos modos pic e full.
        if rel == "sce_sys/pic0.png" and not WITH_PIC:
            continue

        files.append((targ, ROOT + "/" + rel))

        parts = targ.split("/")[:-1]

        for i in range(1, len(parts) + 1):
            dirs.add("/".join(parts[:i]))


# Validacao dos arquivos essenciais.
names = [target for target, _ in files]

required_files = (
    "eboot.bin",
    "sce_sys/param.sfo",
    "sce_sys/icon0.png",
    "sce_module/libc.prx",
)

for need in required_files:
    if need not in names:
        sys.exit("ERRO: falta image0/" + need)

if "directpayload.bin" not in names:
    sys.exit(
        "ERRO: falta image0/directpayload.bin. "
        "O payload do instalador nao sera incluido no PKG."
    )

if "sce_sys/about/right.sprx" not in names:
    print(
        "AVISO: image0/sce_sys/about/right.sprx nao encontrado "
        "(o exemplo hello_world inclui esse arquivo)"
    )


def tree(prefix, depth):
    out = ""

    children = sorted(
        item
        for item in dirs
        if item.count("/") == depth and item.startswith(prefix)
    )

    for directory in children:
        name = directory.split("/")[-1]
        inner = tree(directory + "/", depth + 1)
        pad = "  " * (depth + 2)

        if inner:
            out += (
                pad
                + '<dir targ_name="%s">\n%s%s</dir>\n'
                % (name, inner, pad)
            )
        else:
            out += pad + '<dir targ_name="%s"/>\n' % name

    return out


file_xml = "".join(
    '    <file targ_path="%s" orig_path="%s"/>\n' % (target, original)
    for target, original in sorted(files)
)

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

print(
    "project.gp4 gerado (%d arquivos, MODE=%s, content_id=%s)"
    % (len(files), MODE, CONTENT_ID)
)
