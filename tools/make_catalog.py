#!/usr/bin/env python3
"""Gera image0/catalog.dat (catalogo do app nativo) a partir de image0/index.html.

O index.html continua sendo a unica fonte: cards (.game-card), capas, textos, categorias
(<select id="category">) e botoes (a.download-button com data-pkg, data-name, data-content-id,
data-size, data-type). Rode tools/pkg_info.py ANTES para preencher ID/tamanho/tipo.

Botoes sem link http(s) em data-pkg (placeholders) sao ignorados.
Uso: python3 tools/make_catalog.py [index.html] [catalog.dat]
"""
import os
import re
import sys
from html.parser import HTMLParser


def clean(s):
    return re.sub(r"\s+", " ", (s or "").replace("\t", " ")).strip()


VOID = {"img", "br", "hr", "input", "meta", "link", "source", "wbr"}


class Parser(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.cats = []          # (value, label)
        self.games = []
        self.game = None
        self.depth = 0          # profundidade dentro do <article>
        self.cap = None         # (campo, buffer) em captura de texto
        self.btn = None
        self.in_option = None

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        cls = (a.get("class") or "").split()
        if tag == "option" and self.game is None:
            self.in_option = [a.get("value", ""), ""]
        if tag == "article" and "game-card" in cls:
            self.game = {"category": a.get("data-category", ""), "cover": "", "tag": "",
                         "kicker": "", "title": "", "desc": "", "btns": [],
                         "name": a.get("data-name", "")}
            self.depth = 1
            return
        if self.game is None:
            return
        if tag not in VOID:
            self.depth += 1
        if tag == "img" and "cover" in cls:
            self.game["cover"] = os.path.basename(a.get("src", ""))
        elif tag == "span" and "card-tag" in cls:
            self.cap = ["tag", ""]
        elif tag == "p" and "card-kicker" in cls:
            self.cap = ["kicker", ""]
        elif tag == "h3":
            self.cap = ["title", ""]
        elif tag == "p" and "card-description" in cls:
            self.cap = ["desc", ""]
        elif tag == "a" and "download-button" in cls:
            self.btn = {"url": a.get("data-pkg", "").strip(), "name": a.get("data-name", ""),
                        "id": a.get("data-content-id", ""), "size": a.get("data-size", "0"),
                        "type": a.get("data-type", ""), "base": "base-download-button" in cls,
                        "label": ""}
            self.cap = ["label", ""]

    def handle_data(self, data):
        if self.in_option is not None:
            self.in_option[1] += data
        if self.cap is not None:
            self.cap[1] += data

    def handle_endtag(self, tag):
        if tag == "option" and self.in_option is not None:
            v, l = self.in_option
            self.cats.append((v, clean(l)))
            self.in_option = None
        if self.game is None or tag in VOID:
            return
        if self.cap is not None:
            field, text = self.cap
            ended = (field == "label" and tag == "a") or (field != "label" and tag in ("span", "p", "h3"))
            if ended:
                text = clean(text)
                if field == "label":
                    text = clean(re.sub(r"[↓⬇▼]+\s*$", "", text))   # a seta e desenhada pelo app
                    self.btn["label"] = text
                    self.game["btns"].append(self.btn)
                    self.btn = None
                else:
                    self.game[field] = text
                self.cap = None
        self.depth -= 1
        if tag == "article" and self.depth <= 0:
            self.games.append(self.game)
            self.game = None


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join("image0", "index.html")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join("image0", "catalog.dat")
    root = os.path.dirname(src) or "."

    p = Parser()
    p.feed(open(src, encoding="utf-8").read())
    if not p.games:
        sys.exit("ERRO: nenhum .game-card encontrado em " + src)

    lines = ["# catalog.dat - gerado por tools/make_catalog.py (nao edite; edite o index.html)"]
    cats = p.cats or [("all", "Todas")]
    for v, l in cats:
        lines.append("CAT\t%s\t%s" % (v, l))

    total_btn = skipped = 0
    for g in p.games:
        cover = g["cover"]
        if cover and not os.path.exists(os.path.join(root, cover)):
            print("AVISO: capa nao encontrada: %s (card '%s')" % (cover, g["title"]))
        lines.append("GAME\t%s\t%s\t%s\t%s\t%s\t%s" % (
            g["category"], cover, g["tag"], g["kicker"], g["title"], g["desc"]))
        for b in g["btns"]:
            if not re.match(r"https?://", b["url"]):
                skipped += 1
                print("AVISO: botao '%s' (%s) sem link http(s) em data-pkg: ignorado" % (b["label"], g["title"]))
                continue
            size = re.sub(r"\D", "", b["size"]) or "0"
            lines.append("BTN\t%s\t%s\t%s\t%s\t%s\t%s\t%d" % (
                b["label"], clean(b["name"]) or g["title"], b["url"], b["id"], size,
                b["type"], 1 if b["base"] else 0))
            total_btn += 1

    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print("catalog.dat: %d cards, %d botoes, %d categorias (%d botoes sem link ignorados)"
          % (len(p.games), total_btn, len(cats), skipped))


if __name__ == "__main__":
    main()
