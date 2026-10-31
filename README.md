# JK Patchs (PS4)

App nativo para PS4 (OpenOrbis): interface propria em 1920x1080 desenhada pelo app, catalogo embutido,
controle para navegar e instalacao dos patches pelo payload do DirectPackageInstaller.

Estrutura: `image0/` (catalogo `index.html`, capas, `sce_sys`), `src/` (codigo do app), `assets/fonts`,
`tools/` (geradores). O build monta `native0/` e gera **um unico pacote**: `out/native/*.pkg`.

## Compilar na nuvem (sem instalar nada)
1. Aba **Actions** > **build-pkg** > **Run workflow** (tambem roda a cada push).
2. Baixe o artefato **ps4-patch-store-pkg** e instale `out/native/*.pkg` (app "JK Patchs").
3. `build.log` e `diag.log` vao junto no artefato.

Local (Linux/WSL): `OO_PS4_TOOLCHAIN=/caminho/PS4Toolchain bash build.sh` (resultado em `out/native/`).

## Controles
Setas ou analogico esquerdo movem, X instala, O volta ao inicio, L1/R1 trocam de categoria.

## Adicionando um patch novo
1. No `image0/index.html`, crie o botao so com o link do PKG:
   `<a class="download-button" href="#" data-pkg="https://.../MEU%20PATCH.pkg" data-name="Nome">...</a>`
2. Pronto: o build roda `tools/pkg_info.py`, que le so o cabecalho do PKG (poucos KB, via HTTP Range)
   e preenche `data-content-id`, `data-size` e `data-type`. Depois `tools/make_catalog.py` gera o
   catalogo do app a partir do mesmo `index.html`. Nao precisa digitar ID nem tamanho.
3. Pacotes `PS4GD` (jogo) e `PS4GP` (patch) convivem no mesmo catalogo; o tipo e repassado ao payload.
   Tipo vazio ou invalido vira `PS4GD` e gera um aviso em `/data/ps4store.log`.
4. Botao sem `data-pkg` e ignorado. Link fora do ar ou que nao seja PKG: o build avisa e o botao fica como esta.

Conferir um PKG sem alterar nada: `python3 tools/pkg_info.py --show "https://.../MEU.pkg"`.
Preencher o index local sem gerar o app: `tools\atualizar_catalogo.bat` (Windows) ou
`python3 tools/pkg_info.py image0/index.html`. A hospedagem precisa aceitar `Range` (o R2 aceita).

## Nome, icone e fundo
- Nome no PS4: `TITLE` em `tools/make_sfo.py` (bloco `APP == "native"`). Nome dentro do app: `src/ui.c`.
- Icone (`image0/sce_sys/icon0.png`, 512x512) e fundo (`pic0.png`, 1920x1080) sao gerados por
  `python3 tools/make_icon.py` (bola de futebol, estadio e nome; requer pillow e numpy). Para usar uma arte
  sua, basta trocar esses dois PNG pelos seus (mesmos tamanhos).

## Diagnostico e testes
- Log no PS4: `/data/ps4store.log` ("Video ok.", "Catalogo carregado.", "Controle aberto", "UI pronta." mostram
  ate onde o app chegou). `diag.log` do artefato lista bibliotecas e simbolos do app.
- Interface no PC, sem PS4: `bash tools/sim/run_sim.sh` (gera capturas em `build/sim/out/`).
- Codigo: `src/ps4_main.c` (tela, controle, thread de instalacao), `src/ui.c` (interface), `src/gfx.c`
  (desenho), `src/catalog.c`, `src/install.c` (payload DPI). Fonte: DejaVu Sans (licenca livre em `assets/fonts`).
- `src/main.c`, `image0/app.js` e `image0/style.css` eram da versao navegador e nao sao mais usados no build.

## Licenca e creditos
O payload `directpayload.bin`, baixado no `build.sh`, vem do projeto
[DirectPackageInstaller](https://github.com/marcussacana/DirectPackageInstaller) (Marcus Sacana),
sob **GNU GPL v3**. Ao distribuir o app com o payload incluso, a GPL v3 vale para essa parte:
mantenha o aviso de licenca, indique a origem e disponibilize o codigo-fonte correspondente
(o do payload e o das suas modificacoes). O texto completo esta no `LICENSE` do DirectPackageInstaller.
