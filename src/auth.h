/* Login e catalogo pela rede (HTTPS -> Cloudflare Worker). Tudo roda em uma thread propria; a UI consulta o
 * resultado com auth_poll() (nao bloqueia). A verificacao do certificado do servidor fica SEMPRE ligada. */
#ifndef AUTH_H
#define AUTH_H

enum { AUTH_EV_NONE = 0,
       AUTH_EV_NEED_LOGIN,     /* nao ha sessao salva (ou expirou): mostrar a tela de login */
       AUTH_EV_LOGIN_FAIL,     /* msg = motivo */
       AUTH_EV_CATALOG,        /* text = catalogo (malloc; quem recebe libera) ; user = usuario */
       AUTH_EV_CATALOG_FAIL,   /* msg = motivo (sem rede, etc.); a sessao salva e mantida */
       AUTH_EV_REGISTER_OK,    /* cadastro enviado; a conta fica pendente ate o admin ativar */
       AUTH_EV_REGISTER_FAIL };/* msg = motivo (usuario ja existe, etc.) */

typedef struct {
  int type;
  char msg[200];
  char user[64];
  char plan[48];          /* AUTH_EV_CATALOG: plano do usuario ("ilimitado" ou data ISO); vazio se o servidor nao informou */
  long plan_now;          /* hora do servidor (segundos) quando o plano foi lido; 0 = desconhecida */
  char *text;
} AuthEvent;

/* Le a URL do servidor (/app0/server.url ou /data/jkpatchs_server.txt). 0 se achou. */
int auth_server_configured(void);

void auth_start_restore(void);                                  /* tenta a sessao salva e baixa o catalogo */
void auth_start_login(const char *user, const char *pass);      /* login + catalogo */
void auth_start_register(const char *user, const char *pass);   /* cadastro (conta nasce pendente) */
void auth_logout(void);                                         /* apaga a sessao salva */
int  auth_poll(AuthEvent *ev);                                  /* 1 se havia evento (preenche ev) */

/* Baixa uma URL https para um arquivo (video do catalogo). pct 0..100; cancel != 0 interrompe. 0 = ok. */
int  auth_download(const char *url, const char *dest, volatile int *pct, volatile int *cancel, char *err, int cap);

#endif
