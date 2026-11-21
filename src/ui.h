/* Interface do app (independente de plataforma): desenha em uma superficie Image e recebe teclas logicas. */
#ifndef UI_H
#define UI_H

#include <stddef.h>
#include "gfx.h"
#include "catalog.h"

enum { UI_UP = 1, UI_DOWN, UI_LEFT, UI_RIGHT, UI_OK, UI_BACK, UI_L1, UI_R1, UI_OPTIONS, UI_SQUARE, UI_TRIANGLE, UI_L2, UI_R2 };   /* L2/R2: v1.40 */
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
typedef void (*UiPreviewFn)(void *ud);
void ui_set_preview_handler(UI *ui, UiPreviewFn fn);  /* botao "Ver a loja" da tela de login (modo visitante, so olhar) */
void ui_set_register_handler(UI *ui, UiLoginFn fn);   /* botao CADASTRAR da tela de login */
void ui_show_register_done(UI *ui, const char *user);  /* (antigo) cadastro enviado: QR Code do WhatsApp do admin */
/* Planos: janela "Mensal / Semestral / Anual" -> Pix -> "Ja paguei" -> QR do WhatsApp com a mensagem pronta. */
typedef void (*UiReloadFn)(void *ud);
void ui_set_reload_handler(UI *ui, UiReloadFn fn);     /* chamado quando o usuario pede para atualizar o plano */
void ui_set_plans_text(UI *ui, const char *body);      /* resposta do /plans (precos + Pix atuais); vazio = mantem os embutidos */
void ui_open_shop(UI *ui);                             /* abre a janela de planos (se ha conta logada) */
/* Plano da conta (servidor novo): o app calcula o texto do topo, o aviso de 48 h e o bloqueio quando vence durante o uso. */
void ui_set_plan_info(UI *ui, const char *state, const char *plan_name, long expires, long server_now);
void ui_show_loading(UI *ui, const char *msg);
void ui_show_login(UI *ui, const char *msg);          /* msg: erro/aviso (pode ser NULL); limpa a senha */
void ui_set_catalog(UI *ui, const Catalog *cat);      /* o Catalog precisa continuar valido enquanto a UI o usa */
void ui_set_user(UI *ui, const char *name);
void ui_set_offline(UI *ui, int on);                    /* 1 = catalogo salvo no console (servidor fora do ar): downloads indisponiveis */
void ui_remind_support(UI *ui);                        /* lembrete fixo / apoio vencido: abre a janela de planos */
void ui_set_plan(UI *ui, const char *text, int level);   /* linha do plano no topo; text vazio/NULL esconde. level: 0 ok, 1 aviso, 2 vencido */
void ui_key(UI *ui, int key);
void ui_skip_splash(UI *ui);          /* pula a abertura animada (usado pelo simulador) */
void ui_tick(UI *ui);                 /* chamar ~60x por segundo */
int  ui_take_dirty(UI *ui);           /* 1 se precisa redesenhar (e zera a flag) */
void ui_draw(UI *ui);
void ui_set_busy(UI *ui, int busy);
/* Resultado da instalacao pedida (code 0 = pedido aceito). Mostra a mensagem certa (inclusive para a atualizacao do app). */
void ui_install_done(UI *ui, int code, const char *err);   /* code 2 = PKG da atualizacao do app baixado em /data/pkg */
/* Nome do arquivo do PKG do app (a partir do link do catalogo), o mesmo usado ao salvar em /data/pkg. */
void ui_app_pkg_name(const char *url, char *out, size_t n);
/* Video dentro do app: chamar uma vez com os ganchos de plataforma ja registrados em vp_init(). */
void ui_toast(UI *ui, const char *msg, int kind);

#endif
