#!/usr/bin/env bash
# Uso: OO_PS4_TOOLCHAIN=/caminho/toolchain ./build.sh
set -euo pipefail
: "${OO_PS4_TOOLCHAIN:?defina OO_PS4_TOOLCHAIN}"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
python3 tools/make_sfo.py
make
mkdir -p out
PKGTOOL=$(find "$OO_PS4_TOOLCHAIN/bin" -iname 'PkgTool.Core*' -type f | grep -vi '\.dll$' | head -1)
"$PKGTOOL" pkg_build project.gp4 out
ls -la out
