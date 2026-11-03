#!/usr/bin/env python3
"""Converte um video (MP4 etc.) para o formato .jkv que o JK Patchs toca dentro do app.

.jkv = quadros JPEG (MJPEG) + audio IMA-ADPCM mono 24 kHz, intercalados.
Uso: python3 tools/mp4_to_jkv.py entrada.mp4 saida.jkv [largura=480] [fps=15] [qualidade=6] [max_segundos=120]
A entrada pode ser um link https. So os primeiros max_segundos viram .jkv (o app baixa o arquivo inteiro antes de tocar).
Precisa do ffmpeg instalado. Qualidade: 2 (melhor/maior) ... 31 (pior/menor).
"""
import os
import struct
import subprocess
import sys
import tempfile
import glob

ARATE = 24000
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
        97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
        724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
        4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
        18500, 20350, 22385, 24623, 27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_block(samples):
    """Bloco IMA-ADPCM independente: s16 predictor, u8 index, u8 0 + nibbles das amostras 1..n-1."""
    pred = samples[0]
    idx = 0
    out = bytearray(struct.pack("<hBB", pred, idx, 0))
    nibs = []
    for s in samples[1:]:
        step = STEP[idx]
        diff = s - pred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        d = step >> 3
        if diff >= step:
            code |= 4; diff -= step; d += step
        step2 = step >> 1
        if diff >= step2:
            code |= 2; diff -= step2; d += step2
        step4 = step >> 2
        if diff >= step4:
            code |= 1; d += step4
        pred = pred - d if code & 8 else pred + d
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + INDEX[code & 7]))
        nibs.append(code)
    if len(nibs) % 2:
        nibs.append(0)
    for i in range(0, len(nibs), 2):
        out.append(nibs[i] | (nibs[i + 1] << 4))
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    width = int(sys.argv[3]) if len(sys.argv) > 3 else 480
    fps = int(sys.argv[4]) if len(sys.argv) > 4 else 15
    qual = int(sys.argv[5]) if len(sys.argv) > 5 else 6
    maxs = sys.argv[6] if len(sys.argv) > 6 else os.environ.get("JKV_MAX_SEC", "120")
    spf = ARATE // fps
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-t", maxs, "-an", "-vf", "fps=%d,scale=%d:-2" % (fps, width),
                        "-c:v", "mjpeg", "-pix_fmt", "yuvj420p", "-q:v", str(qual), os.path.join(tmp, "f%06d.jpg")],
                       check=True)
        pcm_path = os.path.join(tmp, "a.raw")
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-t", maxs, "-vn", "-ac", "1", "-ar", str(ARATE),
                        "-f", "s16le", pcm_path], check=False)
        pcm = open(pcm_path, "rb").read() if os.path.exists(pcm_path) else b""
        frames = sorted(glob.glob(os.path.join(tmp, "f*.jpg")))
        if not frames:
            sys.exit("ERRO: ffmpeg nao gerou quadros de %s" % src)
        # largura/altura do 1o quadro (SOF0/SOF2)
        j0 = open(frames[0], "rb").read()
        i = 2
        w = h = 0
        while i < len(j0):
            if j0[i] != 0xFF:
                i += 1
                continue
            m = j0[i + 1]
            if m in (0xC0, 0xC1, 0xC2):
                h, w = struct.unpack(">HH", j0[i + 5:i + 9])
                break
            i += 2 + struct.unpack(">H", j0[i + 2:i + 4])[0]
        n = len(frames)
        sam = struct.unpack("<%dh" % (len(pcm) // 2), pcm[:len(pcm) // 2 * 2]) if pcm else ()
        out = bytearray(b"JKV1" + struct.pack("<HHHHII", w, h, fps, ARATE, n, spf))
        for k, fp in enumerate(frames):
            jpg = open(fp, "rb").read()
            chunk = list(sam[k * spf:(k + 1) * spf])
            chunk += [0] * (spf - len(chunk))
            ad = adpcm_block(chunk)
            out += struct.pack("<II", len(jpg), len(ad)) + jpg + ad
    open(dst, "wb").write(out)
    print("%s -> %s  %dx%d  %d quadros @%dfps  %.1f s  %.1f MB" % (src, dst, w, h, n, fps, n / fps, len(out) / 1048576.0))


if __name__ == "__main__":
    main()
