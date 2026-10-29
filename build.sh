#!/usr/bin/env bash
# Uso: OO_PS4_TOOLCHAIN=/caminho/toolchain bash build.sh
# Gera dois pacotes: out/min (so eboot + sce_sys, para testar a instalacao) e out/full (com o site).
set -euo pipefail
: "${OO_PS4_TOOLCHAIN:?defina OO_PS4_TOOLCHAIN}"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
export DOTNET_ROLL_FORWARD=Major

run_pkgtool() {
  local DLL BIN
  DLL=$(find "$OO_PS4_TOOLCHAIN/bin" -name 'PkgTool.Core.dll' | head -1)
  if [ -n "$DLL" ]; then
    echo "Usando dotnet $DLL"
    dotnet "$DLL" "$@"
  else
    BIN=$(find "$OO_PS4_TOOLCHAIN/bin/linux" -type f -name 'PkgTool.Core' | head -1)
    if [ -z "$BIN" ]; then
      echo "PkgTool nao encontrado. Conteudo de bin:"
      find "$OO_PS4_TOOLCHAIN/bin" -maxdepth 2
      exit 1
    fi
    chmod +x "$BIN"
    "$BIN" "$@"
  fi
}

# right.sprx: o exemplo hello_world (que instala) empacota este arquivo.
mkdir -p image0/sce_sys/about
if [ ! -f image0/sce_sys/about/right.sprx ]; then
  SPRX=$(find "$OO_PS4_TOOLCHAIN" -name right.sprx -type f 2>/dev/null | head -1 || true)
  if [ -n "${SPRX:-}" ]; then
    cp "$SPRX" image0/sce_sys/about/right.sprx
    echo "right.sprx copiado de $SPRX"
  else
    echo "AVISO: right.sprx nao encontrado no toolchain"
  fi
fi

python3 tools/make_sfo.py
make

rm -rf out
mkdir -p out/min out/full

MIN=1 python3 tools/make_gp4.py
cp project.gp4 out/project-min.gp4
run_pkgtool pkg_build project.gp4 out/min

python3 tools/make_gp4.py
cp project.gp4 out/project-full.gp4
run_pkgtool pkg_build project.gp4 out/full

ls -laR out
