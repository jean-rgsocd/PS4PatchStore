/* jk_lic.c - ver jk_lic.h. Usa crypto_sign_open e crypto_hash (SHA-512) do TweetNaCl (dominio publico). */
#include "jk_lic.h"
#include "tweetnacl.h"
#include <string.h>

void randombytes(unsigned char *x, unsigned long long n) { (void)x; (void)n; }   /* so geracao de chave usa; aqui nao */

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static uint64_t num(const char *s, size_t n, int *ok) {
  uint64_t v = 0;
  if (!n || n > 19) { *ok = 0; return 0; }
  for (size_t i = 0; i < n; i++) { if (s[i] < '0' || s[i] > '9') { *ok = 0; return 0; } v = v * 10 + (uint64_t)(s[i] - '0'); }
  return v;
}
/* compara versoes "4.16" x "4.2" por partes numericas */
static int cmp_versao(const char *a, size_t na, const char *b, size_t nb) {
  size_t i = 0, j = 0;
  while (i < na || j < nb) {
    unsigned long x = 0, y = 0;
    while (i < na && a[i] >= '0' && a[i] <= '9') x = x * 10 + (unsigned long)(a[i++] - '0');
    while (j < nb && b[j] >= '0' && b[j] <= '9') y = y * 10 + (unsigned long)(b[j++] - '0');
    if (x != y) return x < y ? -1 : 1;
    while (i < na && (a[i] < '0' || a[i] > '9')) i++;
    while (j < nb && (b[j] < '0' || b[j] > '9')) j++;
  }
  return 0;
}

void jk_lic_cid(const char *dev, char out[25]) {
  static const char H[] = "0123456789abcdef";
  unsigned char buf[96], h[64];
  size_t n = 0;
  const char *pre = "jkcid:";
  while (*pre && n < sizeof buf) buf[n++] = (unsigned char)*pre++;
  while (dev && *dev && n < sizeof buf) buf[n++] = (unsigned char)*dev++;
  crypto_hash(h, buf, n);
  for (int i = 0; i < 12; i++) { out[2 * i] = H[h[i] >> 4]; out[2 * i + 1] = H[h[i] & 15]; }
  out[24] = 0;
}

const char *jk_lic_motivo(int c) {
  switch (c) {
    case JK_LIC_OK: return "licenca ok";
    case JK_LIC_FORMATO: return "sem licenca (abra o app JK PATCHS com internet)";
    case JK_LIC_ASSINATURA: return "licenca invalida";
    case JK_LIC_MODULO: return "licenca de outro modulo";
    case JK_LIC_CONSOLE: return "licenca de outro console";
    case JK_LIC_VENCIDA: return "licenca vencida (abra o app JK PATCHS com internet)";
    case JK_LIC_RELOGIO: return "data do console atrasada";
    case JK_LIC_VERSAO: return "versao do plugin desatualizada";
    default: return "licenca invalida";
  }
}

int jk_lic_checar(const char *c, size_t n, const unsigned char pub[32], const char *modulo,
                  const char *versao, const char *dev, uint64_t agora, uint64_t visto, uint64_t *vence_out) {
  if (!c || !pub || !modulo || !dev) return JK_LIC_FORMATO;
  while (n && (c[n - 1] == '\n' || c[n - 1] == '\r' || c[n - 1] == ' ' || c[n - 1] == 0)) n--;
  if (n < 20 || n > 400) return JK_LIC_FORMATO;
  /* separa os 8 campos */
  const char *f[8]; size_t fl[8]; int k = 0; size_t ini = 0;
  for (size_t i = 0; i <= n && k < 8; i++) {
    if (i == n || c[i] == '|') { f[k] = c + ini; fl[k] = i - ini; k++; ini = i + 1; }
  }
  if (k != 8 || ini <= n) return JK_LIC_FORMATO;          /* sobrou texto depois do 8o campo */
  if (fl[0] != 4 || memcmp(f[0], "JKL1", 4)) return JK_LIC_FORMATO;
  if (fl[7] != 128) return JK_LIC_FORMATO;
  /* assinatura: sm = assinatura(64) + payload; payload = tudo antes do ultimo '|' */
  size_t plen = (size_t)(f[7] - c) - 1;
  static unsigned char sm[64 + 400], m[64 + 400];
  for (int i = 0; i < 64; i++) {
    int a = hexval(f[7][2 * i]), b = hexval(f[7][2 * i + 1]);
    if (a < 0 || b < 0) return JK_LIC_FORMATO;
    sm[i] = (unsigned char)(a * 16 + b);
  }
  memcpy(sm + 64, c, plen);
  unsigned long long mlen = 0;
  if (crypto_sign_open(m, &mlen, sm, 64 + plen, pub) != 0 || mlen != plen) return JK_LIC_ASSINATURA;
  /* daqui para baixo os campos sao confiaveis (assinados pelo servidor) */
  size_t ml = strlen(modulo);
  if (fl[1] != ml || memcmp(f[1], modulo, ml)) return JK_LIC_MODULO;
  char cid[25];
  jk_lic_cid(dev, cid);
  if (fl[2] != 24 || memcmp(f[2], cid, 24)) return JK_LIC_CONSOLE;
  int ok = 1;
  uint64_t emitida = num(f[5], fl[5], &ok), vence = num(f[6], fl[6], &ok);
  if (!ok) return JK_LIC_FORMATO;
  if (vence_out) *vence_out = vence;
  if (visto && agora + 86400 < visto) return JK_LIC_RELOGIO;   /* voltou a data em mais de 1 dia */
  if (agora + 86400 < emitida) return JK_LIC_RELOGIO;           /* relogio antes da emissao (tolerancia 1 dia) */
  if (agora > vence) return JK_LIC_VENCIDA;
  if (!(fl[4] == 1 && f[4][0] == '-') && versao && cmp_versao(versao, strlen(versao), f[4], fl[4]) < 0) return JK_LIC_VERSAO;
  return JK_LIC_OK;
}
