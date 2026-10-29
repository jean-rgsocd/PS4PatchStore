#!/usr/bin/env python3
"""Gera image0/sce_sys/param.sfo com os MESMOS campos do exemplo hello_world do OpenOrbis
(que instala e roda no PS4). Sem FORMAT, PARENTAL_LEVEL nem PUBTOOLINFO."""
import os
import struct

TITLE = "PS4 Patch Store"
VERSION = "1.00"
TITLE_ID = "PSTR00001"                                   # 9 caracteres, NAO usar prefixo CUSA
CONTENT_ID = "IV0000-PSTR00001_00-PS4PATCHSTORE000"      # 36 caracteres

OUT = os.path.join("image0", "sce_sys", "param.sfo")


def s(value, maxsize):
    return ("s", value, maxsize)


def i(value):
    return ("i", value, 4)


# Mesmos campos, tipos e tamanhos maximos do hello_world.
ENTRIES = {
    "APP_TYPE": i(1),
    "APP_VER": s(VERSION, 8),
    "ATTRIBUTE": i(0),
    "CATEGORY": s("gd", 4),
    "CONTENT_ID": s(CONTENT_ID, 48),
    "DOWNLOAD_DATA_SIZE": i(0),
    "SYSTEM_VER": i(0),
    "TITLE": s(TITLE, 128),
    "TITLE_ID": s(TITLE_ID, 12),
    "VERSION": s(VERSION, 8),
}


def build():
    keys = sorted(ENTRIES)
    key_table = b""
    key_offset = {}
    for k in keys:
        key_offset[k] = len(key_table)
        key_table += k.encode("ascii") + b"\0"
    while len(key_table) % 4:
        key_table += b"\0"

    data_table = b""
    index = b""
    for k in keys:
        kind, value, maxsize = ENTRIES[k]
        if kind == "s":
            raw = value.encode("utf-8") + b"\0"
            fmt, length = 0x0204, len(raw)
            raw = raw.ljust(maxsize, b"\0")
        else:
            raw = struct.pack("<I", value)
            fmt, length = 0x0404, 4
        index += struct.pack("<HHIII", key_offset[k], fmt, length, maxsize, len(data_table))
        data_table += raw

    key_start = 20 + len(index)
    data_start = key_start + len(key_table)
    header = b"\0PSF" + struct.pack("<IIII", 0x101, key_start, data_start, len(keys))
    return header + index + key_table + data_table


def dump(blob):
    magic, ver, kstart, dstart, count = struct.unpack("<4sIIII", blob[:20])
    assert magic == b"\0PSF", "magic invalido"
    lines = ["Entry Name : Entry Type(Size / Max Size) = Entry Value"]
    for n in range(count):
        ko, fmt, ln, mx, do = struct.unpack("<HHIII", blob[20 + n * 16: 36 + n * 16])
        name = blob[kstart + ko: blob.index(b"\0", kstart + ko)].decode()
        raw = blob[dstart + do: dstart + do + ln]
        if fmt == 0x0404:
            val, kind = "0x%08X" % struct.unpack("<I", raw)[0], "Integer"
        else:
            val, kind = raw.rstrip(b"\0").decode(), "Utf8"
        lines.append("%s : %s(%d/%d) = %s" % (name, kind, ln, mx, val))
    return "\n".join(lines)


if __name__ == "__main__":
    assert len(TITLE_ID) == 9 and len(CONTENT_ID) == 36
    blob = build()
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(blob)
    print(dump(blob))
