/* jk_modulo.c - ver jk_modulo.h */
#include "jk_modulo.h"
#include "tweetnacl.h"
#include <string.h>

static int hx(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
int jkm_chave_hex(const char *t, size_t n, unsigned char k[32]) {
  while (n && (t[n - 1] == '\n' || t[n - 1] == '\r' || t[n - 1] == ' ' || t[n - 1] == 0)) n--;
  if (n != 64) return 1;
  for (int i = 0; i < 32; i++) {
    int a = hx(t[2 * i]), b = hx(t[2 * i + 1]);
    if (a < 0 || b < 0) return 1;
    k[i] = (unsigned char)(a * 16 + b);
  }
  return 0;
}

int jkm_ler(const unsigned char *b, size_t n, jkm_info *o) {
  size_t p = 0;
  if (n < 4 + 1 + 1 + 24 + 4 + 16 || memcmp(b, "JKM1", 4)) return JKM_FORMATO;
  p = 4;
  unsigned nm = b[p++];
  if (!nm || nm > 40 || p + nm >= n) return JKM_FORMATO;
  memcpy(o->modulo, b + p, nm); o->modulo[nm] = 0; p += nm;
  for (unsigned i = 0; i < nm; i++) {
    char c = o->modulo[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return JKM_FORMATO;
  }
  unsigned nt = b[p++];
  if (nt > 160 || p + nt + 24 + 4 + 16 > n) return JKM_FORMATO;
  memcpy(o->titulos, b + p, nt); o->titulos[nt] = 0; p += nt;
  o->nonce = b + p; p += 24;
  o->tamanho = (uint32_t)b[p] | ((uint32_t)b[p + 1] << 8) | ((uint32_t)b[p + 2] << 16) | ((uint32_t)b[p + 3] << 24); p += 4;
  o->hash16 = b + p; p += 16;
  if (o->tamanho == 0 || o->tamanho > 64u * 1024 * 1024 || p + o->tamanho != n) return JKM_FORMATO;
  o->cifrado = b + p;
  return JKM_OK;
}

int jkm_titulo_ok(const jkm_info *i, const char *t) {
  size_t tl = strlen(t);
  if (!tl) return 0;
  const char *s = i->titulos;
  while (*s) {
    const char *v = strchr(s, ',');
    size_t l = v ? (size_t)(v - s) : strlen(s);
    if (l == tl && !memcmp(s, t, l)) return 1;
    if (l == 1 && s[0] == '*') return 1;
    if (!v) break;
    s = v + 1;
  }
  return 0;
}

int jkm_decifrar(const jkm_info *i, const unsigned char chave[32], unsigned char *saida) {
  unsigned char h[64];
  crypto_stream_xor(saida, i->cifrado, i->tamanho, i->nonce, chave);
  crypto_hash(h, saida, i->tamanho);
  if (memcmp(h, i->hash16, 16)) { memset(saida, 0, i->tamanho); return JKM_CHAVE; }   /* chave errada ou arquivo alterado */
  return JKM_OK;
}

size_t jkm_tamanho_cifrado(const char *m, const char *t, uint32_t tam) {
  return 4 + 1 + strlen(m) + 1 + strlen(t) + 24 + 4 + 16 + tam;
}
size_t jkm_montar(const char *m, const char *t, const unsigned char nonce[24], const unsigned char chave[32],
                  const unsigned char *prx, uint32_t tam, unsigned char *o) {
  size_t p = 0, nm = strlen(m), nt = strlen(t);
  unsigned char h[64];
  memcpy(o, "JKM1", 4); p = 4;
  o[p++] = (unsigned char)nm; memcpy(o + p, m, nm); p += nm;
  o[p++] = (unsigned char)nt; memcpy(o + p, t, nt); p += nt;
  memcpy(o + p, nonce, 24); p += 24;
  o[p++] = (unsigned char)(tam & 255); o[p++] = (unsigned char)((tam >> 8) & 255); o[p++] = (unsigned char)((tam >> 16) & 255); o[p++] = (unsigned char)(tam >> 24);
  crypto_hash(h, prx, tam);
  memcpy(o + p, h, 16); p += 16;
  crypto_stream_xor(o + p, prx, tam, nonce, chave);
  return p + tam;
}
