#!/usr/bin/env python3
"""Gera image0/sce_sys/param.sfo (PS4, category gd). Uso: python3 tools/make_sfo.py"""
import struct

CONTENT_ID = "IV0000-CUSA99999_00-PS4PATCHSTORE000"   # exatamente 36 caracteres
assert len(CONTENT_ID) == 36

UTF8_S, UTF8, INT32 = 0x0004, 0x0204, 0x0404
# (chave, formato, valor, max_len)
ENTRIES = [
    ("APP_TYPE",           INT32, 0,                 4),
    ("APP_VER",            UTF8,  "01.00",           8),
    ("ATTRIBUTE",          INT32, 0,                 4),
    ("CATEGORY",           UTF8,  "gd",              4),
    ("CONTENT_ID",         UTF8,  CONTENT_ID,        48),
    ("DOWNLOAD_DATA_SIZE", INT32, 0,                 4),
    ("FORMAT",             UTF8_S, "obs",            4),
    ("PARENTAL_LEVEL",     INT32, 0,                 4),
    ("PUBTOOLINFO",        UTF8,  "c_date=20261029,sdk_ver=05050000", 512),
    ("SYSTEM_VER",         INT32, 0,                 4),
    ("TITLE",              UTF8,  "PS4 Patch Store", 128),
    ("TITLE_ID",           UTF8,  "CUSA99999",       12),
    ("VERSION",            UTF8,  "01.00",           8),
]

def build(entries):
    entries = sorted(entries, key=lambda e: e[0])
    keys, offs = b"", []
    for k, *_ in entries:
        offs.append(len(keys)); keys += k.encode() + b"\0"
    while len(keys) % 4: keys += b"\0"
    ktab = 20 + 16 * len(entries)
    dtab = ktab + len(keys)
    idx, data = b"", b""
    for (k, fmt, val, mx), ko in zip(entries, offs):
        if fmt == INT32:
            raw = struct.pack("<I", val)
        else:
            raw = val.encode() + (b"\0" if fmt == UTF8 else b"")
        assert len(raw) <= mx, k
        idx += struct.pack("<HHIII", ko, fmt, len(raw), mx, len(data))
        data += raw.ljust(mx, b"\0")
    head = struct.pack("<4sIIII", b"\0PSF", 0x101, ktab, dtab, len(entries))
    return head + idx + keys + data

if __name__ == "__main__":
    with open("image0/sce_sys/param.sfo", "wb") as f:
        f.write(build(ENTRIES))
    print("param.sfo gerado.")
