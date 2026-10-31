# PS4 Patch Store (PKG)

Estrutura: `image0/` (conteudo do app0), `src/main.c` (eboot), `tools/make_sfo.py` e `tools/make_gp4.py`
(geram `param.sfo` e `project.gp4` a cada build).

## Compilar na nuvem (sem instalar nada)
1. Aba **Actions** > **build-pkg** > **Run workflow** (tambem roda a cada push).
2. Baixe o artefato **ps4-patch-store-pkg**. Ele traz dois pacotes:
   - `out/min/*.pkg`: so o launcher (sem site e sem pic0). **Instale este primeiro** para testar.
   - `out/full/*.pkg`: launcher + site.
3. `build.log` e `diag.log` vao junto no artefato.

## App nativo (sem navegador)
Alem dos pacotes acima, o build gera `out/native/*.pkg`: o **PS4 Patch Store Nativo**
(`PSTR00002`). A interface e desenhada pelo proprio app (1920x1080, por software), com o
catalogo embutido; o controle navega e o X instala. Ele instala **ao lado** da versao navegador
(`PSTR00001`), sem substitui-la, entao da para comparar e voltar atras.

- O catalogo vem do mesmo `image0/index.html` (`tools/make_catalog.py` le cards, capas, categorias e
  botoes depois do `pkg_info.py`). Nao existe segundo lugar para editar.
- Fonte: DejaVu Sans (`assets/fonts`, licenca livre incluida). Codigo: `src/ps4_main.c` (tela, controle,
  thread de instalacao), `src/ui.c` (interface), `src/gfx.c` (desenho), `src/install.c` (payload DPI).
- Diagnostico: `/data/ps4store.log` (linhas "Video ok.", "Catalogo carregado.", "UI pronta." mostram ate onde
  o app chegou). `diag.log` do artefato lista as bibliotecas e simbolos do app.
- Controles: setas/analogico esquerdo movem, X instala, O volta ao inicio, L1/R1 trocam de categoria.
- Testar a interface no PC, sem PS4: `bash tools/sim/run_sim.sh` (gera capturas em `build/sim/out/`).

## Local (Linux/WSL)
`OO_PS4_TOOLCHAIN=/caminho/PS4Toolchain bash build.sh`  (resultado em `out/`)

## Antes de gerar
Edite `image0/index.html` e preencha `data-pkg="URL"` nos botoes. Prefira links `http://`.

## Adicionando um patch novo
1. No `image0/index.html`, crie o botao so com o link do PKG:
   `<a class="download-button" href="#" data-pkg="https://.../MEU%20PATCH.pkg" data-name="Nome">...</a>`
2. Pronto: o `build.sh` (e o workflow) roda `tools/pkg_info.py`, que le so o cabecalho do PKG
   (poucos KB, via HTTP Range) e preenche `data-content-id`, `data-size` e `data-type`.
   Nao precisa digitar ID nem tamanho.
3. O `app.js` envia o tipo ao `eboot`, que o repassa ao payload, entao pacotes `PS4GD` (jogo)
   e `PS4GP` (patch) convivem no mesmo catalogo. Tipo vazio ou invalido vira `PS4GD` e gera um
   aviso em `/data/ps4store.log`.
4. Botao sem `data-pkg` e ignorado. Link fora do ar ou que nao seja PKG: o build avisa e o botao
   fica como esta.

Para conferir um PKG sem alterar nada: `python3 tools/pkg_info.py --show "https://.../MEU.pkg"`.
Para preencher o index local sem gerar o app: `tools\atualizar_catalogo.bat` (Windows) ou
`python3 tools/pkg_info.py image0/index.html`. A hospedagem precisa aceitar `Range` (o R2 aceita).

## Licenca e creditos
O payload `directpayload.bin`, baixado no `build.sh`, vem do projeto
[DirectPackageInstaller](https://github.com/marcussacana/DirectPackageInstaller) (Marcus Sacana),
sob **GNU GPL v3**. Ao distribuir o app com o payload incluso, a GPL v3 vale para essa parte:
mantenha o aviso de licenca, indique a origem e disponibilize o codigo-fonte correspondente
(o do payload e o das suas modificacoes). O texto completo esta no `LICENSE` do DirectPackageInstaller.

## Controle
Direcional move o foco, X aciona, Bola volta ao topo, Triangulo busca, Quadrado/R1 proxima categoria, L1 anterior.
Teclado no PC: setas, Enter, Esc, T, S, Q/E.
