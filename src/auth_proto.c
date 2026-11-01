#include "auth_proto.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t auth_urlencode(const char *s, char *out, size_t cap) {
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;
  if (cap == 0) return 0;
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      if (n + 1 >= cap) break;
      out[n++] = (char)c;
    } else {
      if (n + 3 >= cap) break;
      out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15];
    }
  }
  out[n] = 0;
  return n;
}

static size_t enc_len(const char *s) {
  size_t n = 0;
  for (; *s; s++) n += (isalnum((unsigned char)*s) || strchr("-_.~", *s)) ? 1 : 3;
  return n;
}

int auth_build_login_body(const char *user, const char *pass, char *out, size_t cap) {
  if (enc_len(user) + enc_len(pass) + 12 > cap) return -1;
  size_t n = 5;
  memcpy(out, "user=", 5);
  n += auth_urlencode(user, out + n, cap - n);
  memcpy(out + n, "&pass=", 6); n += 6;
  auth_urlencode(pass, out + n, cap - n);
  return 0;
}

char *auth_trim(char *s) {
  while (*s && isspace((unsigned char)*s)) s++;
  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
  return s;
}

static void put(char *dst, size_t cap, const char *src) {
  if (!cap) return;
  snprintf(dst, cap, "%s", src);
}

int auth_parse_login(int http, const char *body, char *token, size_t tcap, long *exp, char *err, size_t ecap) {
  if (!body) body = "";
  if (http == 200 && !strncmp(body, "OK\n", 3)) {
    const char *t = body + 3;
    const char *e = strchr(t, '\n');
    size_t len = e ? (size_t)(e - t) : strlen(t);
    if (len == 0 || len >= tcap) { put(err, ecap, "Resposta do servidor inválida."); return -1; }
    memcpy(token, t, len);
    token[len] = 0;
    for (size_t i = 0; i < len; i++)                    /* token vai em cabecalho HTTP: so caracteres seguros */
      if (!isalnum((unsigned char)token[i]) && !strchr("-_.=+/", token[i])) {
        put(err, ecap, "Resposta do servidor inválida."); return -1;
      }
    if (exp) *exp = e ? atol(e + 1) : 0;
    return 0;
  }
  if (!strncmp(body, "ERRO\n", 5)) {
    char tmp[200];
    put(tmp, sizeof(tmp), body + 5);
    char *nl = strchr(tmp, '\n'); if (nl) *nl = 0;
    put(err, ecap, auth_trim(tmp));
    if (!err[0]) put(err, ecap, "Falha no login.");
    return -1;
  }
  if (http == 429) put(err, ecap, "Muitas tentativas. Aguarde alguns minutos.");
  else if (http == 401 || http == 403) put(err, ecap, "Usuário ou senha inválidos.");
  else if (http >= 500) put(err, ecap, "Servidor indisponível. Tente mais tarde.");
  else snprintf(err, ecap, "Resposta inesperada do servidor (HTTP %d).", http);
  return -1;
}
