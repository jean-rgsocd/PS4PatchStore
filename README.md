# PS4 Patch Store (PKG)

Estrutura: `image0/` (conteúdo do app0), `src/main.c` (eboot), `tools/make_sfo.py`, `project.gp4`.

## Jeito mais fácil: compilar na nuvem (sem instalar nada)
1. Suba esta pasta inteira para um repositório no GitHub.
2. Aba **Actions** > **build-pkg** > **Run workflow**.
3. Baixe o `.pkg` em *Artifacts* e instale no PS4 (Remote Package Installer ou pendrive).

## Local (Linux/WSL)
`OO_PS4_TOOLCHAIN=/caminho/PS4Toolchain ./build.sh`  (resultado em `out/`)

## Antes de gerar
Edite `image0/index.html` e preencha `data-pkg="URL"` nos botões de patch.
