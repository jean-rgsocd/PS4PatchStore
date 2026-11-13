/* Parte pura (sem rede) do protocolo de login: codificacao de formulario e leitura das respostas do servidor. */
#ifndef AUTH_PROTO_H
#define AUTH_PROTO_H

#include <stddef.h>

/* Codifica s como valor de formulario (application/x-www-form-urlencoded). Retorna o tamanho escrito. */
size_t auth_urlencode(const char *s, char *out, size_t cap);

/* Monta "user=..&pass=..". Retorna 0 ou -1 se nao couber. */
int auth_build_login_body(const char *user, const char *pass, char *out, size_t cap);

/* Igual a auth_build_login_body, mais "&dev=<id do console>" (dev vazio = nao envia). */
int auth_build_login_body_dev(const char *user, const char *pass, const char *dev, char *out, size_t cap);

/* Le a resposta do /login. http = codigo HTTP. Retorna 0 (login ok: preenche token) ou -1 (err com o motivo). */
int auth_parse_login(int http, const char *body, char *token, size_t tcap, long *exp, char *err, size_t ecap);

/* Le a resposta do /me: "OK\nusuario\nplano\nestado\nagora". plano = "ilimitado" ou data ISO; *now = hora do servidor
 * em segundos (0 se ausente). Retorna 0 ou -1. */
int auth_parse_me(int http, const char *body, char *plan, size_t pcap, long *now);

/* Resposta completa do /me (linhas: OK, usuario, plano, estado, agora, apoio, lembrete, vencimento(s), nome do plano). */
typedef struct {
  char plan[48];        /* "ilimitado", data ISO ou "pendente" */
  char state[16];       /* ativo, vencido, pendente, desativado (vazio = servidor antigo) */
  long now;             /* hora do servidor (s) */
  long support;         /* apoio ate (s) - servidor antigo */
  int nag;              /* lembrete fixo - servidor antigo */
  long expires;         /* vencimento do plano (s); 0 = sem vencimento */
  char plan_name[16];   /* trial, mensal, semestral, anual, vitalicio, legado */
} AuthMe;
int auth_parse_me2(int http, const char *body, AuthMe *m);

/* Texto do plano no topo do app. Retorna o nivel: 0 normal, 1 aviso (teste gratis / faltam 48 h ou menos), 2 vencido. */
int auth_plan_status(const char *state, const char *plan_name, long expires, long now, char *out, size_t cap);

/* Planos de assinatura (resposta do /plans). */
typedef struct { char id[16]; char name[24]; char price[16]; int days; char pix[320]; } AuthPlan;
typedef struct { char wa[24]; int trial_min; int n; AuthPlan plan[4]; } AuthPlans;
int auth_parse_plans(const char *body, AuthPlans *out);      /* 0 = ok (pelo menos 1 plano) */

/* "15.00" -> "R$ 15,00" */
void auth_money(const char *price, char *out, size_t cap);

/* Monta o texto do topo (ex.: "PLANO ATIVO  ·  EXPIRA 04/12/2026"). Retorna o nivel: 0 normal, 1 aviso (faltam 5 dias
 * ou menos), 2 vencido, ou -1 se o plano e invalido (nada a mostrar). now = hora do servidor (0 = so mostra a data). */
int auth_plan_text(const char *plan, long now, char *out, size_t cap);

/* Remove espacos/quebras no fim e no inicio (altera a string). */
char *auth_trim(char *s);

#endif
