/* Interface do app (independente de plataforma): desenha em uma superficie Image e recebe teclas logicas. */
#ifndef UI_H
#define UI_H

#include "gfx.h"
#include "catalog.h"

enum { UI_UP = 1, UI_DOWN, UI_LEFT, UI_RIGHT, UI_OK, UI_BACK, UI_L1, UI_R1, UI_OPTIONS, UI_SQUARE, UI_TRIANGLE };
enum { UI_TOAST_INFO = 0, UI_TOAST_OK = 1, UI_TOAST_ERR = 2 };

typedef struct UI UI;
/* Chamada quando o usuario pede uma acao (instalar PKG ou baixar arquivo). Deve retornar rapido. */
typedef void (*UiInstallFn)(const CatButton *b, void *user);

/* Login: o app chama ui_show_login()/ui_show_loading() e, ao receber o catalogo do servidor, ui_set_catalog(). */
typedef void (*UiLoginFn)(const char *user, const char *pass, void *ud);
typedef void (*UiLogoutFn)(void *ud);
/* Abre um link https (video) no navegador do PS4. */
typedef void (*UiOpenUrlFn)(const char *url, void *ud);
/* Baixa uma imagem https para o arquivo dest (chamado fora da thread principal). Retorna 0 se deu certo. */
typedef int (*UiCoverFetchFn)(const char *url, const char *dest, void *ud);

/* cat pode ser NULL (o app comeca na tela de login/carregando e recebe o catalogo depois). */
UI  *ui_create(Image *surface, const Catalog *cat, const char *assets_dir, UiInstallFn fn, void *user);
void ui_set_auth_handlers(UI *ui, UiLoginFn login, UiLogoutFn logout, void *ud);
void ui_set_open_handler(UI *ui, UiOpenUrlFn fn, void *ud);
/* Capas remotas: dir = pasta onde as imagens baixadas ficam guardadas (cache entre aberturas do app). */
void ui_set_cover_fetcher(UI *ui, UiCoverFetchFn fn, const char *dir, void *ud);
void ui_set_register_handler(UI *ui, UiLoginFn fn);   /* botao CADASTRAR da tela de login */
void ui_show_register_done(UI *ui, const char *user);  /* cadastro enviado: mostra o QR Code do WhatsApp do admin */
void ui_show_loading(UI *ui, const char *msg);
void ui_show_login(UI *ui, const char *msg);          /* msg: erro/aviso (pode ser NULL); limpa a senha */
void ui_set_catalog(UI *ui, const Catalog *cat);      /* o Catalog precisa continuar valido enquanto a UI o usa */
void ui_set_user(UI *ui, const char *name);
void ui_set_plan(UI *ui, const char *text, int level);   /* linha do plano no topo; text vazio/NULL esconde. level: 0 ok, 1 aviso, 2 vencido */
void ui_key(UI *ui, int key);
void ui_skip_splash(UI *ui);          /* pula a abertura animada (usado pelo simulador) */
void ui_tick(UI *ui);                 /* chamar ~60x por segundo */
int  ui_take_dirty(UI *ui);           /* 1 se precisa redesenhar (e zera a flag) */
void ui_draw(UI *ui);
void ui_set_busy(UI *ui, int busy);
/* Resultado da instalacao pedida (code 0 = pedido aceito). Mostra a mensagem certa (inclusive para a atualizacao do app). */
void ui_install_done(UI *ui, int code, const char *err);
/* Video dentro do app: chamar uma vez com os ganchos de plataforma ja registrados em vp_init(). */
void ui_toast(UI *ui, const char *msg, int kind);

#endif
