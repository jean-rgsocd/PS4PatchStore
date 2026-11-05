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

/* ------------------------------------------------------------------ plano (/me) */
int auth_parse_me(int http, const char *body, char *plan, size_t pcap, long *now) {
  if (now) *now = 0;
  if (plan && pcap) plan[0] = 0;
  if (http != 200 || !body || strncmp(body, "OK\n", 3) != 0 || !plan || pcap < 2) return -1;
  const char *p = strchr(body + 3, '\n');                /* pula a linha do usuario */
  if (!p) return -1;
  p++;
  const char *e = strchr(p, '\n');
  size_t n = e ? (size_t)(e - p) : strlen(p);
  if (n == 0 || n >= pcap) return -1;
  for (size_t i = 0; i < n; i++)
    if (!isalnum((unsigned char)p[i]) && !strchr(":.-", p[i])) return -1;
  memcpy(plan, p, n);
  plan[n] = 0;
  if (e && now) {
    const char *st = strchr(e + 1, '\n');                /* pula a linha do estado */
    if (st) *now = atol(st + 1);
  }
  return 0;
}

static long days_from_civil(long y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static void civil_from_days(long z, int *y, int *m, int *d) {
  z += 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  long doe = z - era * 146097;
  long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long yy = yoe + era * 400;
  long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  long mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yy + (*m <= 2));
}

int auth_plan_text(const char *plan, long now, char *out, size_t cap) {
  if (!out || cap == 0) return -1;
  out[0] = 0;
  if (!plan || !plan[0]) return -1;
  if (!strcmp(plan, "ilimitado")) { snprintf(out, cap, "PLANO ILIMITADO"); return 0; }

  int Y, M, D, h = 0, mi = 0, sec = 0;
  if (sscanf(plan, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &sec) < 3) return -1;
  if (Y < 2020 || Y > 2100 || M < 1 || M > 12 || D < 1 || D > 31) return -1;
  long exp = days_from_civil(Y, M, D) * 86400L + h * 3600L + mi * 60L + sec;

  long loc = exp - 3 * 3600L;                              /* data no horario de Brasilia (igual ao painel) */
  int y, m, d;
  civil_from_days(loc >= 0 ? loc / 86400 : (loc - 86399) / 86400, &y, &m, &d);
  char date[16];
  snprintf(date, sizeof(date), "%02d/%02d/%04d", d, m, y);

  if (now < 1700000000L) {                                 /* sem hora confiavel: so a data */
    snprintf(out, cap, "PLANO ATIVO  \xC2\xB7  EXPIRA %s", date);
    return 0;
  }
  long diff = exp - now;
  if (diff < 0) { snprintf(out, cap, "PLANO VENCIDO  \xC2\xB7  %s", date); return 2; }
  long days = (diff + 86399) / 86400;                      /* arredonda para cima, como o painel */
  if (days <= 5) {
    if (days <= 0) snprintf(out, cap, "PLANO EXPIRA HOJE  \xC2\xB7  %s", date);
    else snprintf(out, cap, "PLANO EXPIRA EM %ld %s  \xC2\xB7  %s", days, days == 1 ? "DIA" : "DIAS", date);
    return 1;
  }
  snprintf(out, cap, "PLANO ATIVO  \xC2\xB7  EXPIRA %s", date);
  return 0;
}
