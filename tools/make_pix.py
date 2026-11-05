#!/usr/bin/env python3
"""Gera image0/qr_pix.png: o QR Code do seu Pix (BR Code estatico) com a chave escrita embaixo.
Uso:  python3 tools/make_pix.py CHAVE "NOME DE QUEM RECEBE" "CIDADE" [--valor 10] [--out arquivo.png]
  CHAVE: celular (com DDD, ex.: 11999999999 ou +5511999999999), e-mail, CNPJ, chave aleatoria; para CPF use a opcao --cpf.
  Sem --valor, quem paga digita o valor (recomendado para ajuda voluntaria). Com --valor 10, o QR ja vem com R$ 10,00.
Ou use o codigo "Copia e Cola" que o seu banco gerou (e o mais seguro):
  python3 tools/make_pix.py --codigo "00020126...6304XXXX"
Precisa de opencv (cv2) e Pillow. Se o arquivo image0/qr_pix.png nao existir, o app nao mostra o botao de apoio."""
import re, sys, unicodedata
import cv2
from PIL import Image, ImageDraw, ImageFont

FONT_B = "/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf"
FONT_M = "/usr/share/fonts/truetype/google-fonts/Poppins-Medium.ttf"

def ascii_up(s, n):
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode().upper()
    return re.sub(r"[^A-Z0-9 ]", "", s).strip()[:n] or "JK"

def tlv(i, v): return "%s%02d%s" % (i, len(v), v)

def crc16(data):
    crc = 0xFFFF
    for b in data.encode("ascii"):
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return "%04X" % crc

def norm_key(k, cpf=False):
    k = k.strip()
    if "@" in k: return k.lower()
    d = re.sub(r"\D", "", k)
    if k.startswith("+"): return "+" + d                                  # celular ja com +55
    if len(d) == 14 or (len(d) == 11 and cpf): return d                   # CNPJ / CPF (CPF so com --cpf)
    if len(d) in (10, 11): return "+55" + d                               # celular com DDD
    return k                                                              # chave aleatoria (UUID)

def payload(key, name, city, valor=None):
    mai = tlv("00", "br.gov.bcb.pix") + tlv("01", key)
    p = tlv("00", "01") + tlv("01", "11") + tlv("26", mai) + tlv("52", "0000") + tlv("53", "986")
    if valor: p += tlv("54", "%.2f" % valor)
    p += tlv("58", "BR") + tlv("59", ascii_up(name, 25)) + tlv("60", ascii_up(city, 15)) + tlv("62", tlv("05", "***"))
    p += "6304"
    return p + crc16(p)

def main():
    a = sys.argv[1:]
    out = "image0/qr_pix.png"; valor = None
    if "--out" in a: i = a.index("--out"); out = a[i + 1]; del a[i:i + 2]
    if "--valor" in a: i = a.index("--valor"); valor = float(a[i + 1].replace(",", ".")); del a[i:i + 2]
    if "--codigo" in a:                                   # usa o codigo do banco como esta (so confere o CRC)
        i = a.index("--codigo"); code = a[i + 1].strip(); del a[i:i + 2]
        body = code[:-4]
        if crc16(body) != code[-4:].upper(): print("ERRO: o CRC do codigo nao confere (copie o codigo inteiro)."); sys.exit(1)
        m = re.search(r"0014br\.gov\.bcb\.pix01(\d\d)", code)
        key = code[m.end():m.end() + int(m.group(1))] if m else ""
        v = re.search(r"54(\d\d)(\d+\.\d\d)", code)
        return render(code, key, out, "R$ " + v.group(2).replace(".", ",") if v else None)
    cpf = "--cpf" in a
    if cpf: a.remove("--cpf")
    if len(a) < 3: print(__doc__); sys.exit(2)
    key = norm_key(a[0], cpf); code = payload(key, a[1], a[2], valor)
    render(code, key, out, ("R$ %.2f" % valor).replace(".", ",") if valor else None)
    print("nome:", ascii_up(a[1], 25), "| cidade:", ascii_up(a[2], 15))

def render(code, key, out, valor_txt):
    qr = cv2.QRCodeEncoder_create().encode(code)
    mods = qr.shape[0]
    scale = max(4, 400 // mods)
    qr = cv2.resize(qr, None, fx=scale, fy=scale, interpolation=cv2.INTER_NEAREST)
    qimg = Image.fromarray(qr).convert("RGB")
    W = 520; pad = 40
    img = Image.new("RGB", (W, qimg.height + 2 * pad + 70), (255, 255, 255))
    img.paste(qimg, ((W - qimg.width) // 2, pad))
    d = ImageDraw.Draw(img)
    shown = key if len(key) <= 26 else key[:23] + "..."
    top = "Chave Pix" + ("  \u00b7  " + valor_txt if valor_txt else "")
    d.text((W // 2, qimg.height + pad + 20), top, font=ImageFont.truetype(FONT_M, 20), fill=(90, 100, 120), anchor="mm")
    d.text((W // 2, qimg.height + pad + 52), shown, font=ImageFont.truetype(FONT_B, 28), fill=(10, 20, 40), anchor="mm")
    img.save(out, optimize=True)
    print("ok", out, "(%dx%d, QR %d modulos)" % (img.width, img.height, mods))
    print("chave:", key, "| valor:", valor_txt or "livre")
    print("codigo:", code)

if __name__ == "__main__":
    main()
