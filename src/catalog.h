/* Catalogo embutido no app (arquivo catalog.dat gerado por tools/make_catalog.py). */
#ifndef CATALOG_H
#define CATALOG_H

#define CAT_MAX_CATS  16
#define CAT_MAX_GAMES 64
#define CAT_MAX_BTNS  8

typedef struct { char id[32]; char label[64]; } CatCategory;

typedef struct {
  char label[96];          /* texto do botao, ex.: "BAIXAR UPDATE 9.5" */
  char name[200];          /* nome mostrado nas notificacoes do PS4 */
  char url[1024];
  char content_id[48];
  unsigned long long size;
  char type[8];            /* PS4GD, PS4GP, PS4AC... (vazio = padrao) */
  int base;                /* 1 = botao de jogo base (estilo diferente) */
} CatButton;

typedef struct {
  char category[32];
  char cover[96];          /* nome do arquivo em assets/images */
  char tag[32];
  char kicker[96];
  char title[96];
  char desc[400];
  int nbtn;
  CatButton btn[CAT_MAX_BTNS];
} CatGame;

typedef struct {
  int ncat;
  CatCategory cat[CAT_MAX_CATS];   /* cat[0] e sempre "all" */
  int ngame;
  CatGame game[CAT_MAX_GAMES];
} Catalog;

/* Le o catalogo. Retorna 0 em sucesso, <0 em erro (arquivo ausente/vazio). */
int catalog_load(Catalog *c, const char *path);
/* Mesmo parser sobre texto em memoria (usado pelo simulador e pelos testes). */
int catalog_parse(Catalog *c, char *text);

#endif
