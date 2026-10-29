#!/usr/bin/env bash
# Uso: OO_PS4_TOOLCHAIN=/caminho/toolchain bash build.sh
set -euo pipefail
: "${OO_PS4_TOOLCHAIN:?defina OO_PS4_TOOLCHAIN}"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
export DOTNET_ROLL_FORWARD=Major

python3 tools/make_sfo.py
make
mkdir -p out

DLL=$(find "$OO_PS4_TOOLCHAIN/bin" -name 'PkgTool.Core.dll' | head -1)
if [ -n "$DLL" ]; then
  echo "Usando dotnet $DLL"
  dotnet "$DLL" pkg_build project.gp4 out
else
  BIN=$(find "$OO_PS4_TOOLCHAIN/bin/linux" -type f -name 'PkgTool.Core' | head -1)
  if [ -z "$BIN" ]; then
    echo "PkgTool nao encontrado. Conteudo de bin:"
    find "$OO_PS4_TOOLCHAIN/bin" -maxdepth 2
    exit 1
  fi
  chmod +x "$BIN"
  "$BIN" pkg_build project.gp4 out
fi
ls -la out
