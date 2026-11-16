#!/usr/bin/env python3
"""Regenera a tela do painel dentro do worker.js a partir de painel_fonte/painel.html.
Uso (dentro da pasta worker/):  python3 painel_fonte/montar_painel.py
Edite SEMPRE o painel.html e rode este script: nao edite o texto gigante PANEL_HTML no worker.js."""
import json, re, pathlib
aqui = pathlib.Path(__file__).resolve().parent
w = aqui.parent / "worker.js"
html = (aqui / "painel.html").read_text(encoding="utf-8")
html = (html.replace("%%LOGO%%", "/jkpainel17/logo.png")
            .replace("%%R2PUBLIC%%", "https://pub-c6518eb1477949a58d959f5a769703d1.r2.dev/")
            .replace("%%PROTEGIDOS%%", json.dumps(["APP-PS4/JKPATCHS.pkg", "APP-PS4/APP.mp4"])))
src = w.read_text(encoding="utf-8")
novo, n = re.subn(r'const PANEL_HTML = ".*";\n', lambda m: "const PANEL_HTML = " + json.dumps(html) + ";\n", src, count=1)
if n != 1:
    raise SystemExit("ERRO: nao achei a linha const PANEL_HTML no worker.js")
w.write_text(novo, encoding="utf-8")
print("worker.js atualizado (%d bytes)" % len(novo.encode("utf-8")))
