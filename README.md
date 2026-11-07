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
4. Outros botoes podem usar `data-video-url="https://.../tutorial.jkv"` para abrir vídeo `.jkv` no player interno, `href="https://.../guia.png"` para abrir imagem no navegador do PS4, ou `href="https://.../arquivo.cpk"` para baixar um arquivo sem tentar instalar. Também é possível indicar `data-action="download"`, `data-action="image"` ou `data-action="video"`. Downloads comuns são salvos em `/data/PS4PatchStore/downloads/`.
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

## Login e catalogo pelo servidor

- O app abre na tela de login (teclado na tela: direcional move, X digita, quadrado apaga, triangulo troca de
  campo, OPTIONS entra). A sessao fica salva no console (`/data/jkpatchs.sess`, validade definida pelo servidor)
  e o app entra direto nas proximas vezes. No catalogo, triangulo = sair da conta.
- O catalogo **nao vai dentro do PKG**: o app baixa de `GET /catalog` do Cloudflare Worker depois do login.
  Cada build gera `out/catalogo_para_servidor.dat` (baixe no artifact do GitHub Actions) — cole o conteudo em
  "Catalogo do app" na pagina `/admin` do Worker.
- `server.url` (raiz do projeto) = endereco `https://...workers.dev` do Worker. So `https://` e aceito e o
  certificado do servidor e verificado (nunca desativado). Para trocar o endereco sem recompilar, grave o
  mesmo texto em `/data/jkpatchs_server.txt` no console.
- Diagnostico: `/data/ps4store.log` mostra "Rede iniciada", `http: /login -> HTTP 200` e, se falhar, o codigo
  (`0x8095....` = certificado recusado; `0x8043....` = sem conexao/DNS).
- Teclado: letras minusculas/maiusculas, numeros e `_ . - @` (e com MAIUSC: `! # $ % ^ & * ( ) = : , + /`).
  Crie as senhas na pagina /admin usando esses caracteres.
- Aviso: os links dos PKGs no catalogo ainda apontam para o R2 publico; a protecao final e tornar o bucket
  privado e servir links temporarios (proxima etapa).

## Atualizacao do app (o app baixa o PKG sozinho)

O PS4 nao instala um app por cima de outro. Por isso o app so BAIXA o PKG novo (em `/data/pkg/`) e mostra o passo a passo na tela:
a pessoa apaga o app e instala o PKG pelo Package Installer do GoldHEN (Debug Settings > Game > Package Installer). O login fica em `/data` e continua valendo.
1. Aumente `APP_VERSION` em `src/version.h` (ex.: "1.02" -> "1.03"). Esse numero tambem vai para o `param.sfo`.
2. `app.url` (raiz do projeto) = link publico do PKG do app no R2 (ex.: `https://pub-xxxx.r2.dev/APP-PS4/JKPATCHS.pkg`).
3. Faca o build e pegue o PKG de `out/native/`. Suba no R2 **com o nome que esta no `app.url`**, substituindo o antigo.
4. So depois de subir o PKG, cole o `catalogo_para_servidor.dat` (com a linha `APP`) no `/admin`.
5. Quem abrir o app com versao menor: o app baixa o PKG sozinho (barra de aviso em baixo) e □ mostra o passo a passo.
Depois que a pessoa instala a versao nova, o app apaga o PKG baixado de `/data/pkg/`. So suba o catalogo com versao maior depois que o PKG estiver no R2.
