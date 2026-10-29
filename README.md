# PS4 Patch Store (PKG)

Estrutura: `image0/` (conteudo do app0), `src/main.c` (eboot), `tools/make_sfo.py` e `tools/make_gp4.py`
(geram `param.sfo` e `project.gp4` a cada build).

## Compilar na nuvem (sem instalar nada)
1. Aba **Actions** > **build-pkg** > **Run workflow** (tambem roda a cada push).
2. Baixe o artefato **ps4-patch-store-pkg**. Ele traz dois pacotes:
   - `out/min/*.pkg`: so o launcher (sem site e sem pic0). **Instale este primeiro** para testar.
   - `out/full/*.pkg`: launcher + site.
3. `build.log` e `diag.log` vao junto no artefato.

## Local (Linux/WSL)
`OO_PS4_TOOLCHAIN=/caminho/PS4Toolchain bash build.sh`  (resultado em `out/`)

## Antes de gerar
Edite `image0/index.html` e preencha `data-pkg="URL"` nos botoes. Prefira links `http://`.

## Controle
Direcional move o foco, X aciona, Bola volta ao topo, Triangulo busca, Quadrado/R1 proxima categoria, L1 anterior.
Teclado no PC: setas, Enter, Esc, T, S, Q/E.
