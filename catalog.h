/* Catalogo embutido no app (arquivo catalog.dat gerado por tools/make_catalog.py). */
#ifndef CATALOG_H
#define CATALOG_H

#define CAT_MAX_CATS  16
#define CAT_MAX_GAMES 64
#define CAT_MAX_BTNS  20

typedef struct { char id[32]; char label[64]; } CatCategory;

typedef struct {
  char label[96];          /* texto do botao, ex.: "BAIXAR UPDATE 9.5" */
  char name[200];          /* nome mostrado nas notificacoes do PS4 */
  char url[1024];
  char content_id[48];
  unsigned long long size;
  char type[8];            /* PS4GD, PS4GP, PS4AC... (vazio = padrao) */
  char action[12];         /* pkg (padrao), download, video ou image */
  int base;                /* 1 = botao de jogo base (estilo diferente) */
} CatButton;

typedef struct {
  char category[32];
  char cover[256];         /* nome do arquivo em assets/images OU link https de uma imagem (baixada e guardada no console) */
  char tag[32];
  char kicker[96];
  char title[96];
  char desc[400];
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

typedef struct {
  CatApp app;
  int ncat;
  CatCategory cat[CAT_MAX_CATS];   /* cat[0] e sempre "all" */
  int ngame;
  CatGame game[CAT_MAX_GAMES];
} Catalog;

/* Le o catalogo. Retorna 0 em sucesso, <0 em erro (arquivo ausente/vazio). */
int catalog_load(Catalog *c, const char *path);
/* "1.01" -> 101 (0 se invalido). Serve para comparar versoes. */
int catalog_version_num(const char *v);
/* Mesmo parser sobre texto em memoria (usado pelo simulador e pelos testes). */
int catalog_parse(Catalog *c, char *text);

#endif
