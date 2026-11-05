/* Parte pura (sem rede) do protocolo de login: codificacao de formulario e leitura das respostas do servidor. */
#ifndef AUTH_PROTO_H
#define AUTH_PROTO_H

#include <stddef.h>

/* Codifica s como valor de formulario (application/x-www-form-urlencoded). Retorna o tamanho escrito. */
size_t auth_urlencode(const char *s, char *out, size_t cap);

/* Monta "user=..&pass=..". Retorna 0 ou -1 se nao couber. */
int auth_build_login_body(const char *user, const char *pass, char *out, size_t cap);

/* Le a resposta do /login. http = codigo HTTP. Retorna 0 (login ok: preenche token) ou -1 (err com o motivo). */
int auth_parse_login(int http, const char *body, char *token, size_t tcap, long *exp, char *err, size_t ecap);

/* Le a resposta do /me: "OK\nusuario\nplano\nestado\nagora". plano = "ilimitado" ou data ISO; *now = hora do servidor
 * em segundos (0 se ausente). Retorna 0 ou -1. */
int auth_parse_me(int http, const char *body, char *plan, size_t pcap, long *now);

/* Monta o texto do topo (ex.: "PLANO ATIVO  ·  EXPIRA 04/12/2026"). Retorna o nivel: 0 normal, 1 aviso (faltam 5 dias
 * ou menos), 2 vencido, ou -1 se o plano e invalido (nada a mostrar). now = hora do servidor (0 = so mostra a data). */
int auth_plan_text(const char *plan, long now, char *out, size_t cap);

/* Remove espacos/quebras no fim e no inicio (altera a string). */
char *auth_trim(char *s);

#endif
