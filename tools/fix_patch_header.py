#!/usr/bin/env python3
"""Ajusta o cabecalho de um PKG de teste para ficar como o de um patch real (content_type/flags) e refaz o SHA-256 do cabecalho.
Uso: fix_patch_header.py ARQ.pkg [content_type_hex] [content_flags_hex] [promote_hex]"""
import hashlib, struct, sys
p = sys.argv[1]
ctype = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
flags = int(sys.argv[3], 16) if len(sys.argv) > 3 else None
promote = int(sys.argv[4], 16) if len(sys.argv) > 4 else None
pfsfl = int(sys.argv[5], 16) if len(sys.argv) > 5 else None
d = bytearray(open(p, "rb").read())
if d[:4] != b"\x7fCNT":
    sys.exit("nao e PKG de PS4")
if ctype is not None: d[0x74:0x78] = struct.pack(">I", ctype)
if flags is not None: d[0x78:0x7c] = struct.pack(">I", flags)
if promote is not None: d[0x7c:0x80] = struct.pack(">I", promote)
if pfsfl is not None: d[0x408:0x410] = struct.pack(">Q", pfsfl)   # pfs_flags: 0xA0000000000003CC nos patches reais (0x80... nos apps)
d[0xFE0:0x1000] = hashlib.sha256(bytes(d[:0xFE0])).digest()
open(p, "wb").write(d)
print("%s: pfs_flags=0x%016X content_type=0x%02X flags=0x%08X promote=0x%08X" % (p, struct.unpack(">Q", d[0x408:0x410])[0], struct.unpack(">I", d[0x74:0x78])[0], struct.unpack(">I", d[0x78:0x7c])[0], struct.unpack(">I", d[0x7c:0x80])[0]))
