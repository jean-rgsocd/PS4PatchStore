#!/usr/bin/env python3
"""Gera a linha MOD do catalogo (versao do modulo jkui.prx + link + tamanho).
Uso: python3 tools/make_mod_line.py <jkui.prx> <app.url> <catalogo_para_servidor.dat> <linha_MOD.txt>
  - versao: src/version.h (APP_VERSION)
  - link: pasta do app.url + MODULO/jkui.prx (no R2: APP-PS4/MODULO/jkui.prx)"""
import os, re, sys
prx, url_file, cat, out = sys.argv[1:5]
ver = re.search(r'APP_VERSION\s+"([0-9]+\.[0-9]{2})"', open("src/version.h").read()).group(1)
base = open(url_file, encoding="utf-8").read().strip().splitlines()[0].strip()
base = base.rsplit("/", 1)[0]
line = "MOD\t%s\t%s/MODULO/jkui.prx\t%d" % (ver, base, os.path.getsize(prx))
open(out, "w", encoding="utf-8").write(line + "\n")
if os.path.exists(cat):
    t = [l for l in open(cat, encoding="utf-8").read().splitlines() if not l.startswith("MOD\t")]
    t.append(line)
    open(cat, "w", encoding="utf-8").write("\n".join(t) + "\n")
print("Linha MOD:", line)
