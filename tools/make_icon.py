#!/usr/bin/env python3
"""Gera o icone (icon0.png 512x512) e o fundo (pic0.png 1920x1080) do app JK Patchs.

Arte 100% original, desenhada por codigo: bola de futebol (icosaedro truncado projetado em 3D),
gramado, luzes de estadio e o nome. Mesma paleta do app (azul-escuro + verde).
Uso: python3 tools/make_icon.py          (grava image0/sce_sys/icon0.png e pic0.png)
Requer: pillow, numpy.
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FONT_BOLD = os.path.join(ROOT, "assets", "fonts", "DejaVuSans-Bold.ttf")
FONT_REG = os.path.join(ROOT, "assets", "fonts", "DejaVuSans.ttf")
OUT_DIR = os.path.join(ROOT, "image0", "sce_sys")

BG_TOP = (8, 13, 24)
BG_MID = (14, 28, 56)
ACCENT = (66, 230, 164)
WHITE = (244, 247, 255)
MUTED = (161, 174, 196)


# ------------------------------------------------------------------ bola de futebol
def _icosahedron():
    p = (1 + 5 ** 0.5) / 2
    v = []
    for a in (-1, 1):
        for b in (-p, p):
            v += [(0, a, b), (a, b, 0), (b, 0, a)]
    v = np.array(v, dtype=float)
    return v / np.linalg.norm(v, axis=1, keepdims=True)


def _rot(ax, ay, az):
    cx, sx, cy, sy, cz, sz = math.cos(ax), math.sin(ax), math.cos(ay), math.sin(ay), math.cos(az), math.sin(az)
    rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return rz @ ry @ rx


def _toward(v, w, ang):
    """Ponto a 'ang' radianos de v, seguindo o grande circulo em direcao a w."""
    t = w - np.dot(w, v) * v
    t = t / np.linalg.norm(t)
    return math.cos(ang) * v + math.sin(ang) * t


def render_ball(d, rot=(-0.55, 0.35, 0.3), ss=3):
    """Devolve RGBA d x d com a bola (fundo transparente)."""
    n = d * ss
    ys, xs = np.mgrid[0:n, 0:n]
    x = (xs + 0.5 - n / 2) / (n / 2)
    y = (ys + 0.5 - n / 2) / (n / 2)
    r2 = x * x + y * y
    inside = r2 < 1.0
    z = np.sqrt(np.clip(1.0 - r2, 0, 1))
    pts = np.stack([x, -y, z], axis=-1)                  # normal da esfera (y para cima)
    R = _rot(*rot)
    pts = pts @ R                                        # equivale a girar a bola

    ico = _icosahedron()
    ang = math.radians(20.9)                             # raio angular do pentagono
    # pentagonos: vertices ao longo das arestas do icosaedro
    pent_mask = np.zeros(inside.shape, bool)
    seam = np.zeros(inside.shape, bool)
    dots = pts @ ico.T
    nearest = dots.argmax(axis=-1)
    cos_ang = math.cos(ang)
    for i, v in enumerate(ico):
        nb = [w for w in ico if abs(np.dot(v, w) - 1 / 5 ** 0.5) < 1e-6]
        # ordena os vizinhos em torno de v
        e1 = nb[0] - np.dot(nb[0], v) * v
        e1 /= np.linalg.norm(e1)
        e2 = np.cross(v, e1)
        nb.sort(key=lambda w: math.atan2(np.dot(w, e2), np.dot(w, e1)))
        corners = [_toward(v, w, ang) for w in nb]
        m = (nearest == i)
        inside_poly = np.ones(inside.shape, bool)
        for k in range(5):
            a, b = corners[k], corners[(k + 1) % 5]
            c = np.cross(a, b)
            c /= np.linalg.norm(c)
            if np.dot(c, v) < 0:
                c = -c
            inside_poly &= (pts @ c) > 0.0
        pent_mask |= m & inside_poly

    # costuras: trecho central de cada aresta do icosaedro (entre dois pentagonos)
    half_w = 0.016
    for i in range(12):
        for j in range(i + 1, 12):
            if abs(np.dot(ico[i], ico[j]) - 1 / 5 ** 0.5) > 1e-6:
                continue
            q1 = _toward(ico[i], ico[j], ang)
            q2 = _toward(ico[j], ico[i], ang)
            c = np.cross(q1, q2)
            c /= np.linalg.norm(c)
            mid = q1 + q2
            mid /= np.linalg.norm(mid)
            half = math.acos(np.clip(np.dot(q1, q2), -1, 1)) / 2
            dist = np.abs(pts @ c)
            proj = pts - (pts @ c)[..., None] * c
            pn = np.linalg.norm(proj, axis=-1)
            pn[pn == 0] = 1
            along = (proj @ mid) / pn
            seam |= (dist < half_w) & (along > math.cos(half))

    # sombreamento (luz vindo de cima a esquerda)
    light = np.array([-0.45, 0.55, 0.70])
    light /= np.linalg.norm(light)
    n_view = np.stack([x, -y, z], axis=-1)
    diff = np.clip(n_view @ light, 0, 1)
    shade = 0.40 + 0.52 * diff
    spec = np.clip((n_view @ (light + np.array([0, 0, 1])) / 1.8), 0, 1) ** 110
    rim = np.clip(1 - z, 0, 1) ** 2.2

    base = np.zeros(inside.shape + (3,))
    base[:] = (246, 248, 252)
    base[seam] = (70, 78, 96)
    base[pent_mask] = (18, 22, 32)
    col = base * shade[..., None]
    col = col * (1 - 0.35 * rim[..., None])
    col += (spec * 45)[..., None]
    col = np.clip(col, 0, 255)

    alpha = np.clip((1.0 - r2) * n * 0.5, 0, 1) * 255     # borda suave
    rgba = np.dstack([col, alpha]).astype(np.uint8)
    rgba[~inside] = 0
    img = Image.fromarray(rgba, "RGBA")
    return img.resize((d, d), Image.LANCZOS)


# ------------------------------------------------------------------ pecas de cena
def vgrad(w, h, stops):
    """Gradiente vertical; stops = [(pos 0..1, (r,g,b)), ...]."""
    arr = np.zeros((h, w, 3), float)
    for yy in range(h):
        t = yy / max(1, h - 1)
        for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
            if p0 <= t <= p1:
                k = (t - p0) / max(1e-9, p1 - p0)
                arr[yy, :] = [c0[i] + (c1[i] - c0[i]) * k for i in range(3)]
                break
    return Image.fromarray(arr.astype(np.uint8), "RGB").convert("RGBA")


def glow(size, center, radius, color, strength):
    w, h = size
    ys, xs = np.mgrid[0:h, 0:w]
    d = np.sqrt((xs - center[0]) ** 2 + (ys - center[1]) ** 2) / radius
    a = np.clip(1 - d, 0, 1) ** 2 * strength
    layer = np.zeros((h, w, 4), np.uint8)
    layer[..., 0], layer[..., 1], layer[..., 2] = color
    layer[..., 3] = (a * 255).astype(np.uint8)
    return Image.fromarray(layer, "RGBA")


def pitch(size, top, bottom_color=(12, 92, 58), top_color=(30, 168, 104), bands=7):
    """Gramado em perspectiva: faixas que alargam para baixo + linhas brancas."""
    w, h = size
    img = vgrad(w, h - top, [(0, top_color), (1, bottom_color)])
    ph = h - top
    ov = Image.new("RGBA", (w, ph), (0, 0, 0, 0))          # desenha numa camada e compoe (PIL nao mistura alpha em RGBA)
    px = ImageDraw.Draw(ov)
    for i in range(bands):
        y0 = int(ph * (i / bands) ** 1.35)
        y1 = int(ph * ((i + 1) / bands) ** 1.35)
        if i % 2 == 0:
            px.rectangle([0, y0, w, y1], fill=(255, 255, 255, 16))
    # circulo central (elipse) e linha do meio
    cx = w // 2
    lw = max(2, w // 220)
    px.ellipse([cx - w * 0.36, ph * 0.22, cx + w * 0.36, ph * 1.45], outline=(255, 255, 255, 120), width=lw)
    px.line([0, int(ph * 0.22), w, int(ph * 0.22)], fill=(255, 255, 255, 120), width=lw)
    img.alpha_composite(ov)
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    out.paste(img, (0, top))
    return out


def spaced_text(draw, xy, text, font, fill, spacing=0, anchor_center=False):
    widths = [draw.textlength(ch, font=font) for ch in text]
    total = sum(widths) + spacing * (len(text) - 1)
    x, y = xy
    if anchor_center:
        x -= total / 2
    for ch, wd in zip(text, widths):
        draw.text((x, y), ch, font=font, fill=fill)
        x += wd + spacing
    return total


def text_layer(size, items):
    """items: (texto, fonte, tamanho, (cx,y), fill, spacing). Devolve (camada, camada_sombra)."""
    lay = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    for text, fpath, fsize, (cx, y), fill, sp in items:
        f = ImageFont.truetype(fpath, fsize)
        spaced_text(d, (cx, y), text, f, fill, sp, anchor_center=True)
    return lay


def shadow_of(layer, blur, offset, opacity):
    a = layer.split()[3].filter(ImageFilter.GaussianBlur(blur))
    sh = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    sh.putalpha(a.point(lambda v: int(v * opacity)))
    out = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    out.paste(sh, offset, sh)
    return out


def ball_with_shadow(canvas, center, d):
    ball = render_ball(d)
    # sombra no gramado
    sh = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(sh)
    cx, cy = center
    sd.ellipse([cx - d * 0.46, cy + d * 0.40, cx + d * 0.46, cy + d * 0.58], fill=(0, 20, 10, 150))
    sh = sh.filter(ImageFilter.GaussianBlur(d * 0.035))
    canvas.alpha_composite(sh)
    canvas.alpha_composite(glow(canvas.size, (cx, cy), d * 0.95, ACCENT, 0.18))
    canvas.alpha_composite(ball, (int(cx - d / 2), int(cy - d / 2)))


# ------------------------------------------------------------------ icone 512x512
def make_icon():
    S = 512
    img = vgrad(S, S, [(0, BG_TOP), (0.55, BG_MID), (1, BG_TOP)])
    img.alpha_composite(glow((S, S), (60, 10), 300, (170, 210, 255), 0.55))     # refletores do estadio
    img.alpha_composite(glow((S, S), (452, 10), 300, (170, 210, 255), 0.55))
    img.alpha_composite(pitch((S, S), 332))
    ball_with_shadow(img, (S // 2, 292), 236)

    # nome: "JK" grande no alto e "PATCHS" na faixa de gramado
    jk = text_layer((S, S), [("JK", FONT_BOLD, 150, (S // 2, 8), ACCENT, 6)])
    img.alpha_composite(shadow_of(jk, 6, (0, 6), 0.8))
    img.alpha_composite(jk)
    pt = text_layer((S, S), [("PATCHS", FONT_BOLD, 62, (S // 2, 428), WHITE, 7)])
    img.alpha_composite(shadow_of(pt, 4, (0, 4), 0.85))
    img.alpha_composite(pt)

    # moldura fina
    ImageDraw.Draw(img).rectangle([0, 0, S - 1, S - 1], outline=(66, 230, 164, 90), width=3)
    return img.convert("RGB")


# ------------------------------------------------------------------ fundo 1920x1080
def make_pic0():
    W, H = 1920, 1080
    bg_path = os.path.join(ROOT, "image0", "stadium_bg.jpg")
    img = vgrad(W, H, [(0, (13, 24, 48)), (1, BG_TOP)])
    if os.path.exists(bg_path):
        pic = Image.open(bg_path).convert("RGB")
        sc = max(W / pic.width, H / pic.height)
        pic = pic.resize((int(pic.width * sc) + 1, int(pic.height * sc) + 1), Image.LANCZOS)
        l, t = (pic.width - W) // 2, (pic.height - H) // 2
        img.paste(pic.crop((l, t, l + W, t + H)).convert("RGBA"), (0, 0))
    veil = vgrad(W, H, [(0, (8, 13, 24)), (1, (8, 13, 24))])
    veil.putalpha(Image.fromarray(np.linspace(228, 250, H).astype(np.uint8)[:, None].repeat(W, 1), "L"))
    img.alpha_composite(veil)
    img.alpha_composite(glow((W, H), (1400, 520), 760, ACCENT, 0.22))

    ball_with_shadow(img, (1400, 520), 620)

    lay = text_layer((W, H), [
        ("JK", FONT_BOLD, 330, (520, 180), ACCENT, 12),
        ("PATCHS", FONT_BOLD, 150, (520, 590), WHITE, 14),
        ("Patches de futebol para PS4", FONT_REG, 46, (520, 790), MUTED, 2),
    ])
    img.alpha_composite(shadow_of(lay, 10, (0, 8), 0.85))
    img.alpha_composite(lay)
    return img.convert("RGB")


if __name__ == "__main__":
    if len(sys.argv) > 1:
        OUT_DIR = sys.argv[1]            # pasta de saida alternativa (testes)
    os.makedirs(OUT_DIR, exist_ok=True)
    icon = make_icon()
    icon.save(os.path.join(OUT_DIR, "icon0.png"), optimize=True)
    print("icon0.png", icon.size)
    pic = make_pic0()
    pic.save(os.path.join(OUT_DIR, "pic0.png"), optimize=True)
    print("pic0.png", pic.size)
