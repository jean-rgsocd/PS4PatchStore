/* Catalogo embutido no app (arquivo catalog.dat gerado por tools/make_catalog.py). */
#ifndef CATALOG_H
#define CATALOG_H

#define CAT_MAX_CATS  16
#define CAT_MAX_GAMES 32
#define CAT_MAX_BTNS  72

typedef struct { char id[32]; char label[64]; } CatCategory;

typedef struct {
  char label[96];          /* texto do botao, ex.: "BAIXAR UPDATE 9.5" */
  char name[200];          /* nome mostrado nas notificacoes do PS4 */
  char url[1024];
  char content_id[48];
  unsigned long long size;
  char type[8];            /* PS4GD, PS4GP, PS4AC... (vazio = padrao) */
  char extra[4096];        /* arquivos extras do botao, separados por '|' (ate 32). v1.37: + campo 12 da linha BTN; v1.39: 4096 (combo do Mega) */
  char dest[128];          /* pasta de destino do download (vazio = /data/PS4PatchStore/downloads) */
  char action[12];         /* pkg (padrao), download, video ou image */
  int base;                /* 1 = botao de jogo base (estilo diferente) */
} CatButton;

typedef struct {
  char category[32];
  char cover[256];         /* nome do arquivo em assets/images OU link https de uma imagem (baixada e guardada no console) */
  char tag[32];
  char kicker[96];
  char title[96];
  char desc[1200];
  char video[200];         /* link https do video (abre no navegador do PS4); vazio = sem video */
  int nbtn;
  CatButton btn[CAT_MAX_BTNS];
} CatGame;

/* Linha "APP" do catalogo: versao mais recente do proprio app e onde baixar o PKG dela. */
typedef struct {
  char version[16];        /* "1.01"; vazio = sem informacao */
  char url[1024];
  char content_id[48];
  unsigned long long size;
  char type[8];
} CatApp;

/* Linha "MOD": versao mais recente do modulo da interface (jkui.prx), baixado em /data/jk sem reinstalar o app. */
typedef struct {
  char version[16];
  char url[1024];
  unsigned long long size;
} CatMod;

/* ---- v1.37: tela "JK-MOD PLACARES" (card com tag PLACARGRID) ----
 * PLB <quadro> <titulo> <cusa> <pasta destino> <extras comuns> <monte_o_seu 0/1>
 * PLO <quadro> <id> <competicao A|B|CDB|LIB> <emissora> <url do cpk> <tamanho> <imagem do placar> <logo>
 * PLP <quadro> <nome do combo> <id A> <id B> <id CDB> <id LIB> [<id MUN> v1.43, opcional]
 * (o app antigo ignora essas linhas) */
#define CAT_PL_COMPS  6          /* v1.45: + SULA (Sul-Americana, gaveta Euro) */
#define CAT_PL_COMPS_OLD 5          /* v1.43: + MUN (Mundial / Copas, placar "Pes" do jogo). O app antigo ignora PLO MUN */
#define CAT_MAX_PLB   3
#define CAT_MAX_PLO   40
#define CAT_MAX_PLP   16
typedef struct { char id[32]; int comp; char name[48]; char url[256]; unsigned long long size; char img[256]; char logo[256]; } CatPlOpt;
typedef struct { char name[64]; int opt[CAT_PL_COMPS]; } CatPlPack;
typedef struct {
  char id[24], title[48], cusa[16], dest[128];
  char extra[4096];        /* v1.39: 4096 (a base do Mega e grande) */
  int monte;
  int nopt, npack;
  CatPlOpt opt[CAT_MAX_PLO];
  CatPlPack pack[CAT_MAX_PLP];
} CatPlBoard;

/* ---- v1.40: tela JK-MOD'S (abas PLACARES / NARRADORES / MASTER LEAGUE PRO no card PLACARGRID)
 * JKG <quadro> <titulo do card JK-MOD do patch>   -> de onde vem os botoes de narracao e de ML daquele patch
 * JKN <chave> <imagem do card do narrador>        -> ordem = ordem na tela; chave = nome do botao sem acento/espaco
 * JKC <chave> <imagem do card do clube>           -> idem para a Master League
 * (o app antigo ignora essas linhas) */
#define CAT_MAX_ART 48
typedef struct { int kind; char key[48]; char img[256]; } CatArt;     /* kind 0 = narrador, 1 = clube */
typedef struct { char board[24]; char game[96]; } CatJkLink;

/* ---- v1.41: telas do card Duvidas (tag AJUDA)
 * FAQ <titulo> <imagem 1920x1080> <video .jkv opcional>   (ordem = ordem das telas; o app antigo ignora) */
#define CAT_MAX_FAQ 16
typedef struct { char title[96]; char img[256]; char video[256]; } CatFaq;

/* ---- v1.43: aba PLUGINS da tela JK-MOD'S (cada plugin opcional com INSTALAR / DESINSTALAR)
 * JKX <chave> <nome> <imagem do card> <quadros "mega,brasfoot"> <prx do plugins.ini ou -> <apagar ou -> <arquivos>
 *   arquivos: "pasta=https://...?once=TAMANHO" separados por '|'; pasta "@" = pasta AFR do quadro (CPK que vai no DpFileList)
 *   apagar:   pastas/arquivos a mais para apagar ao desinstalar ("@/nome" = dentro da pasta AFR)
 * A mesma chave pode ter varias linhas (uma por patch, quando os links mudam). Ordem = ordem dos cards.
 * (o app antigo ignora essas linhas) */
#define CAT_MAX_JKX 40
typedef struct { char key[32]; char name[64]; char img[256]; char boards[64]; char prx[128]; char del[512]; char files[2048]; char video[256]; } CatJkx;   /* v1.47: video = .jkv (9a coluna, opcional) */

typedef struct {
  int locked;              /* 1 = catalogo "so olhar" (visitante / plano vencido ou pendente): sem links de download */
  CatApp app;
  CatMod mod;
  int ncat;
  CatCategory cat[CAT_MAX_CATS];   /* cat[0] e sempre "all" */
  int ngame;
  CatGame game[CAT_MAX_GAMES];
  int nplb;
  CatPlBoard plb[CAT_MAX_PLB];
  int nart, njk;                   /* v1.40 */
  CatArt art[CAT_MAX_ART];
  CatJkLink jk[CAT_MAX_PLB];
  int nfaq;                        /* v1.41 */
  CatFaq faq[CAT_MAX_FAQ];
  int njkx;                        /* v1.43 */
  CatJkx jkx[CAT_MAX_JKX];
} Catalog;

/* Le o catalogo. Retorna 0 em sucesso, <0 em erro (arquivo ausente/vazio). */
int catalog_load(Catalog *c, const char *path);
/* "1.01" -> 101 (0 se invalido). Serve para comparar versoes. */
int catalog_version_num(const char *v);
/* Mesmo parser sobre texto em memoria (usado pelo simulador e pelos testes). */
int catalog_parse(Catalog *c, char *text);
/* "A" "B" "CDB" "LIB" -> 0..3 (-1 se invalido) */
int catalog_pl_comp(const char *s);
/* v1.40: "Galvão Bueno" -> "galvaobueno" (minusculas, sem acento, so letras e numeros) */
void catalog_key(const char *in, char *out, int cap);

#endif
