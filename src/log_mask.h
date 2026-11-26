/* log_mask.h - mascaramento CENTRAL do log do cliente (/data/ps4store.log). Fase G.
 * Tudo que vai para o log passa por log_mask() dentro de install_log():
 *   - links http(s)://...           -> [link]/<nome do arquivo>   (sem servidor, bucket, pasta, token ou ?query)
 *   - "Bearer <token>" / "t=<token>" -> Bearer [oculto] / t=[oculto]
 *   - enderecos IPv4 (a.b.c.d)        -> [ip]
 *   - sequencias hex de 24+ digitos   -> [id]   (id do console, cid, hashes)
 * Header-only (static) para ser testado no PC: tools/teste_log_mask.c */
#ifndef LOG_MASK_H
#define LOG_MASK_H
#include <string.h>
#include <stddef.h>

static int lm_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int lm_dig(char c) { return c >= '0' && c <= '9'; }
static int lm_fim_link(char c) { return c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"' || c == '\'' || c == '|' || c == '<' || c == '>'; }

static size_t lm_put(char *out, size_t o, size_t cap, const char *s, size_t n) {
  for (size_t i = 0; i < n && o + 1 < cap; i++) out[o++] = s[i];
  return o;
}

/* devolve quantos caracteres de um IPv4 comecam em s (0 se nao for IPv4) */
static size_t lm_ipv4(const char *s) {
  size_t i = 0;
  for (int parte = 0; parte < 4; parte++) {
    size_t d = 0;
    while (lm_dig(s[i]) && d < 4) { i++; d++; }
    if (d == 0 || d > 3) return 0;
    if (parte < 3) { if (s[i] != '.') return 0; i++; }
  }
  if (lm_dig(s[i]) || s[i] == '.') return 0;
  return i;
}

static void log_mask(const char *in, char *out, size_t cap) {
  size_t o = 0;
  if (!cap) return;
  for (size_t i = 0; in[i] && o + 1 < cap;) {
    const char *p = in + i;
    /* links */
    if (!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8)) {
      size_t j = 0, nome = 0, fim;
      while (!lm_fim_link(p[j])) j++;
      fim = j;
      for (size_t k = 0; k < j; k++) if (p[k] == '?') { fim = k; break; }
      for (size_t k = 0; k < fim; k++) if (p[k] == '/') nome = k + 1;
      o = lm_put(out, o, cap, "[link]/", 7);
      if (nome && nome < fim && nome > 8) o = lm_put(out, o, cap, p + nome, fim - nome);
      i += j;
      continue;
    }
    /* tokens */
    if (!strncmp(p, "Bearer ", 7) || !strncmp(p, "t=", 2) || !strncmp(p, "token=", 6)) {
      size_t pre = p[0] == 'B' ? 7 : p[0] == 't' && p[1] == '=' ? 2 : 6;
      if (pre == 2 && i > 0 && ((in[i - 1] >= 'a' && in[i - 1] <= 'z') || (in[i - 1] >= 'A' && in[i - 1] <= 'Z'))) { out[o++] = in[i++]; continue; }
      size_t j = pre;
      while (!lm_fim_link(p[j]) && p[j] != '&' && p[j] != ',') j++;
      if (j > pre) { o = lm_put(out, o, cap, p, pre); o = lm_put(out, o, cap, "[oculto]", 8); i += j; continue; }
    }
    /* IPv4 (so no comeco de "palavra") */
    if (lm_dig(*p) && (i == 0 || (!lm_dig(in[i - 1]) && in[i - 1] != '.'))) {
      size_t n = lm_ipv4(p);
      if (n) { o = lm_put(out, o, cap, "[ip]", 4); i += n; continue; }
    }
    /* ids longos em hex (24+) */
    if (lm_hex(*p) && (i == 0 || !lm_hex(in[i - 1]))) {
      size_t j = 0;
      while (lm_hex(p[j])) j++;
      int tem_letra = 0, tem_dig = 0;
      for (size_t k = 0; k < j; k++) { if (lm_dig(p[k])) tem_dig = 1; else tem_letra = 1; }
      if (j >= 24 && tem_letra && tem_dig) { o = lm_put(out, o, cap, "[id]", 4); i += j; continue; }
      o = lm_put(out, o, cap, p, j);
      i += j;
      continue;
    }
    out[o++] = in[i++];
  }
  out[o] = 0;
}
#endif
