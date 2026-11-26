/* cifra_modulo.c - ferramenta do PC: transforma uma PRX (mesmo sem codigo-fonte) em modulo cifrado do JK Core.
 *   cifra_modulo <entrada.prx> <modulo> <titulos> <saida.jkm> [chave_hex_64]
 *   ex.: cifra_modulo pes21_gol_fora.prx golfora CUSA18676,CUSA22062 golfora.jkm
 * Sem chave: gera uma nova e mostra na tela. Guarde-a: vai no produto da Central ("chave de conteudo").
 * Nunca sobrescreve a saida. Confere decifrando de volta antes de terminar. */
#define _CRT_RAND_S
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "jk_modulo.h"
#include "tweetnacl.h"

void randombytes(unsigned char *x, unsigned long long n) {
  for (unsigned long long i = 0; i < n; i++) { unsigned int r; if (rand_s(&r)) { fputs("ERRO: sem gerador aleatorio\n", stderr); exit(3); } x[i] = (unsigned char)r; }
}

int main(int argc, char **argv) {
  if (argc < 5) { fputs("uso: cifra_modulo <entrada.prx> <modulo> <titulos> <saida.jkm> [chave_hex]\n", stderr); return 1; }
  const char *in = argv[1], *mod = argv[2], *tit = argv[3], *out = argv[4];
  if (strlen(mod) == 0 || strlen(mod) > 40 || strlen(tit) > 160) { fputs("ERRO: modulo (1-40) ou titulos (ate 160) invalidos\n", stderr); return 1; }
  for (const char *c = mod; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '.')) { fputs("ERRO: modulo so com a-z 0-9 _ - .\n", stderr); return 1; }
  FILE *t = fopen(out, "rb"); if (t) { fclose(t); fprintf(stderr, "ERRO: %s ja existe (nao sobrescrevo)\n", out); return 1; }
  FILE *f = fopen(in, "rb"); if (!f) { fprintf(stderr, "ERRO: nao abri %s\n", in); return 1; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n <= 0 || n > 64L * 1024 * 1024) { fputs("ERRO: tamanho invalido\n", stderr); return 1; }
  unsigned char *prx = malloc((size_t)n);
  if (fread(prx, 1, (size_t)n, f) != (size_t)n) { fputs("ERRO: leitura\n", stderr); return 1; }
  fclose(f);
  if (n < 4 || prx[0] != 0x7f || prx[1] != 'E' || prx[2] != 'L' || prx[3] != 'F') {
    /* fself do PS4 comeca com 4F 15 3D 1D; ELF comum com 7F 45 4C 46 */
    if (!(prx[0] == 0x4f && prx[1] == 0x15 && prx[2] == 0x3d && prx[3] == 0x1d)) fputs("AVISO: nao parece ELF/fself de PS4; continuando mesmo assim\n", stderr);
  }
  unsigned char chave[32], nonce[24];
  if (argc > 5) { if (jkm_chave_hex(argv[5], strlen(argv[5]), chave)) { fputs("ERRO: chave precisa ter 64 hex\n", stderr); return 1; } }
  else randombytes(chave, 32);
  randombytes(nonce, 24);
  size_t tam = jkm_tamanho_cifrado(mod, tit, (uint32_t)n);
  unsigned char *o = malloc(tam);
  size_t w = jkm_montar(mod, tit, nonce, chave, prx, (uint32_t)n, o);
  /* confere: le de volta e decifra */
  jkm_info inf; unsigned char *volta = malloc((size_t)n);
  if (w != tam || jkm_ler(o, tam, &inf) != JKM_OK || jkm_decifrar(&inf, chave, volta) != JKM_OK || memcmp(volta, prx, (size_t)n)) { fputs("ERRO: conferencia falhou\n", stderr); return 2; }
  f = fopen(out, "wb");
  if (!f || fwrite(o, 1, tam, f) != tam) { fputs("ERRO: gravacao\n", stderr); return 1; }
  fclose(f);
  printf("modulo=%s titulos=%s prx=%ld bytes jkm=%zu bytes\n", mod, tit, n, tam);
  printf("CHAVE=");
  for (int i = 0; i < 32; i++) printf("%02x", chave[i]);
  printf("\n");
  return 0;
}
