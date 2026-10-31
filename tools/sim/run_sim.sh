#!/usr/bin/env bash
# Gera o catalogo, monta os assets como no pacote e salva capturas da UI em build/sim/.
# Uso (na raiz do projeto): bash tools/sim/run_sim.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
rm -rf build/sim && mkdir -p build/sim/assets/fonts build/sim/assets/images build/sim/out
python3 tools/make_catalog.py image0/index.html build/sim/catalog.dat
cp assets/fonts/*.ttf build/sim/assets/fonts/
for f in image0/*.png image0/*.jpg; do [ -f "$f" ] && cp "$f" build/sim/assets/images/; done
gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
    -o build/sim/sim tools/sim/sim.c src/ui.c src/gfx.c src/catalog.c -lm
build/sim/sim build/sim/assets build/sim/catalog.dat build/sim/out
