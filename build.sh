
#!/usr/bin/env bash
# Uso: OO_PS4_TOOLCHAIN=/caminho/toolchain bash build.sh
# Gera um unico pacote em out/native/: o app nativo JK Patchs.

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

# right.sprx
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

# libc.prx e libSceFios2.prx
mkdir -p image0/sce_module

for M in libc.prx libSceFios2.prx; do
  if [ ! -f "image0/sce_module/$M" ]; then
    SRC=$(find "$OO_PS4_TOOLCHAIN" -iname "$M" -type f 2>/dev/null | head -1 || true)

    if [ -n "${SRC:-}" ]; then
      cp "$SRC" "image0/sce_module/$M"
      echo "$M copiado de $SRC"
    else
      echo "ERRO: $M nao encontrado no toolchain."
      find "$OO_PS4_TOOLCHAIN" -iname '*.prx' -type f | head -20
      exit 1
    fi
  fi
done

# Baixar o payload nativo do DirectPackageInstaller
echo "=== Preparando payload BGFT ==="

PAYLOAD_URL="https://raw.githubusercontent.com/marcussacana/DirectPackageInstaller/DN8/DirectPackageInstaller/DirectPackageInstaller/payload"

mkdir -p image0

curl -fL --retry 3 "$PAYLOAD_URL" -o image0/directpayload.bin

# Validar o payload
python3 - <<'PY'
from pathlib import Path

p = Path("image0/directpayload.bin")
data = p.read_bytes()
marker = b"\xb4" * 6

if not data:
    raise SystemExit("ERRO: payload vazio")

if marker not in data:
    raise SystemExit("ERRO: marcador B4 do payload não encontrado")

print("Payload integrado:", len(data), "bytes")
print("Marcador de conexão encontrado.")
PY

echo "DirectPackageInstaller payload: OK"

# Preencher ID, tamanho e tipo (PS4GD/PS4GP...) de cada botao lendo so o cabecalho dos PKGs.
# Se algum link falhar, o botao fica como esta no index.html e o build continua.
echo "=== Lendo cabecalhos dos PKGs ==="
python3 tools/pkg_info.py image0/index.html \
  || echo "AVISO: algum PKG nao pode ser lido (veja ERRO acima); esses botoes usam os valores do index.html"

# Pasta de saida (so o pacote nativo e gerado)
rm -rf out
mkdir -p out

# ---------------------------------------------------------------------------
# App NATIVO: interface desenhada pelo proprio app (sem navegador).
# Unico pacote gerado: "JK Patchs" (PSTR00002) em out/native/.
# ---------------------------------------------------------------------------
echo "=== App nativo ==="

make native

rm -rf native0/assets native0/sce_sys native0/sce_module
mkdir -p native0/assets/fonts native0/assets/images native0/sce_sys native0/sce_module

cp image0/directpayload.bin native0/directpayload.bin
# O catalogo NAO vai dentro do pacote: o app baixa do servidor depois do login.
# Este arquivo e o que voce cola em "Catalogo do app" na pagina /admin do servidor.
python3 tools/make_catalog.py image0/index.html out/catalogo_para_servidor.dat
# Videos do app: baixa cada MP4 do R2 e converte para .jkv (o player do app toca .jkv). Suba out/videos/*.jkv no R2, pasta JKV/.
python3 tools/convert_videos.py image0/index.html out/videos || echo "AVISO: conversao de videos falhou (veja acima)"
# Endereco do servidor de login (https://...workers.dev). Editado em server.url na raiz do projeto.
if [ -f server.url ]; then cp server.url native0/server.url; else echo "AVISO: server.url ausente; o app mostrara 'servidor nao configurado'"; fi
cp assets/fonts/* native0/assets/fonts/
for IMG in image0/*.png image0/*.jpg; do
  [ -f "$IMG" ] && cp "$IMG" native0/assets/images/
done
cp -r image0/sce_sys/. native0/sce_sys/
cp -r image0/sce_module/. native0/sce_module/

APP=native python3 tools/make_sfo.py
MODE=native python3 tools/make_gp4.py
cp project.gp4 out/project-native.gp4
mkdir -p out/native
run_pkgtool pkg_build project.gp4 out/native

# Pacote de ATUALIZACAO (patch, categoria gp): instala por cima do app ja instalado, sem o DPI recusar
# "Package Already Installed". Este e o PKG que vai no app.url / R2 para os usuarios que ja tem o app.
echo "=== Pacote de atualizacao (patch) ==="
PATCH=1 APP=native python3 tools/make_sfo.py
PATCH=1 MODE=native python3 tools/make_gp4.py
cp project.gp4 out/project-patch.gp4
mkdir -p out/patch
run_pkgtool pkg_build project.gp4 out/patch || echo "AVISO: PkgTool nao gerou o patch (veja o erro acima)"

# Linha APP do catalogo: avisa os usuarios que existe versao nova do app (usa app.url + src/version.h)
echo "=== Linha de atualizacao do app ==="
APKG=$(ls out/patch/*.pkg 2>/dev/null | head -1 || true)
if [ -n "$APKG" ]; then
  python3 tools/make_app_line.py "$APKG" app.url out/catalogo_para_servidor.dat \
    || echo "AVISO: nao foi possivel gerar a linha APP"
fi

# Listar resultados
ls -laR out
