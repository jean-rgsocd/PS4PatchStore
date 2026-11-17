#!/usr/bin/env python3
"""Acrescenta ao catalogo do servidor a linha APP (versao mais nova do proprio app + onde baixar o PKG).
Uso: python3 tools/make_app_line.py <pkg_do_app> <arquivo_app.url> <catalogo_para_servidor.dat>
  - versao: src/version.h (APP_VERSION)
  - link: 1a linha do app.url (URL publica do PKG no R2; o PKG precisa estar la com esse nome)
Sem app.url valido a linha NAO e escrita (o catalogo continua funcionando, so sem aviso de atualizacao)."""
import os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
from pkg_info import pkg_info

def main():
    pkg, url_file, cat = sys.argv[1:4]
    ver = re.search(r'APP_VERSION\s+"([0-9]+\.[0-9]{2})"', open("src/version.h").read()).group(1)
    if os.environ.get("APP_LINE_VERSION"):   # modo launcher: a linha APP e a versao do PKG (launcher), nao a do modulo
        ver = os.environ["APP_LINE_VERSION"]
    info = pkg_info(pkg)
    info["size"] = os.path.getsize(pkg)
    url = ""
    if os.path.exists(url_file):
        url = open(url_file, encoding="utf-8").read().strip().splitlines()[0].strip() if open(url_file).read().strip() else ""
    print("PKG do app: %s  (%d bytes)  id=%s  versao=%s" % (os.path.basename(pkg), info["size"], info["content_id"], ver))
    if not re.match(r"https://", url) or "COLE" in url:
        print("AVISO: app.url ausente/invalido -> linha APP NAO adicionada (usuarios nao verao a atualizacao).")
        print("       Crie app.url na raiz com o link publico do PKG, ex.: https://pub-xxxx.r2.dev/JKPatchs.pkg")
        return
    if os.path.basename(url.split("?")[0]).replace("%20", " ") != os.path.basename(pkg):
        print("AVISO: o link do app.url termina em '%s' mas o PKG gerado se chama '%s'. "
              "Lembre: no R2 o arquivo precisa ter o nome do link (renomeie o PKG ao subir)." % (os.path.basename(url), os.path.basename(pkg)))
    line = "APP\t%s\t%s\t%s\t%d\t%s" % (ver, url, info["content_id"], info["size"], info["type"])
    text = open(cat, encoding="utf-8").read().splitlines()
    text = [l for l in text if not l.startswith("APP\t")]
    text.append(line)
    open(cat, "w", encoding="utf-8").write("\n".join(text) + "\n")
    print("Linha APP adicionada em", cat)

main()
