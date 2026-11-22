/* v1.43: plugins opcionais da aba PLUGINS (JK-MOD'S): estado por jogo, plugins.ini e DpFileList.
 * Estado em /data/jk/plugins.txt, linhas "chave@CUSA=0|1". Sem linha = nunca mexeu (vale o que os botoes ja instalaram). */
#ifndef JKPLUG_H
#define JKPLUG_H
#include "catalog.h"

typedef void (*JkxLog)(const char *msg);
/* lista "?list" (hinos): grava em paths os caminhos dos arquivos da lista (ate max) e devolve quantos. Pode ser NULL. */
typedef int (*JkxListFn)(const char *url, const char *dir, char (*paths)[200], int max);

void jkx_set_root(const char *root);                 /* so o simulador: prefixo dos caminhos ("" no PS4) */
void jkx_set_catalog(const Catalog *c);             /* copia os JKX e os quadros (a thread de download usa a copia) */
int  jkx_state(const char *key, const char *cusa); /* -1 nunca mexeu, 0 desinstalado, 1 instalado pela aba */
void jkx_state_set(const char *key, const char *cusa, int v);
int  jkx_for_board(const CatJkx *x, const char *board);
int  jkx_installed(const CatJkx *x, const CatPlBoard *b);
/* monta o botao de download do plugin para o quadro. 0 = ok */
int  jkx_button(const CatJkx *x, const CatPlBoard *b, CatButton *out);
/* idem para desinstalar (acao "jkxoff") */
void jkx_button_off(const CatJkx *x, const CatPlBoard *b, CatButton *out);
/* depois de todo download: plugins.ini + DpFileList de cada jogo conforme o estado */
void jkx_apply(JkxLog log);
/* arruma so o DpFileList de uma pasta (JK_PLACAR, VTA, Varzea e os CPKs dos plugins) */
void jkx_dp_fix(const char *dir, const char *cusa, JkxLog log);
/* 1 = arquivo de plugin opcional que nao esta instalado pela aba neste jogo (o download dos botoes pula) */
int  jkx_blocked(const char *path);
/* "chave@quadro" do botao: liga o estado 1 (antes do download da aba) */
void jkx_mark_install(const char *keyboard);
/* "chave@quadro": desinstala (desliga no ini, apaga os arquivos, arruma o DpFileList). 0 = ok */
int  jkx_uninstall(const char *keyboard, JkxListFn list, JkxLog log, char *msg, int cap);

/* v1.43d: card FIXO (vem com o placar, sem instalar/desinstalar). Linha JKX com arquivos = "FIXO" e prx = "a.prx|b.prx" */
int  jkx_fixed(const CatJkx *x);
const char *jkx_fixed_note(const CatJkx *x);
const char *jkx_aviso(const CatJkx *x);               /* v1.44: "apagar" = AVISO=https://...png -> sem DESINSTALAR, botao AVISO abre a imagem */          /* "apagar" = FIXO:texto -> texto no lugar do status (ex.: DESATIVADO - TRAVANDO) */
int  jkx_fixed_on(const CatJkx *x, const CatPlBoard *b);   /* 1 = algum prx do card esta no console e ligado no plugins.ini */
/* o download gravou este arquivo: se for o prx de um card FIXO, o proximo jkx_apply liga ele e desliga a versao do outro patch
 * do mesmo jogo (ex.: pes21_fin_vivas_mega x pes21_fin_vivas no CUSA18676) */
void jkx_note_download(const char *path);

/* caminhos no console dos arquivos do plugin (para mostrar na tela) */
int  jkx_paths(const CatJkx *x, const CatPlBoard *b, char (*out)[200], int max);

#endif
