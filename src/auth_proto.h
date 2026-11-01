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

/* Remove espacos/quebras no fim e no inicio (altera a string). */
char *auth_trim(char *s);

#endif
