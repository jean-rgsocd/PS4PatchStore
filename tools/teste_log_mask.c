/* teste_log_mask.c - prova que o log do cliente nao vaza links, bucket, chaves do R2, tokens, IPs e ids.
 * Compilar no PC: cc -I../src teste_log_mask.c -o teste_log_mask   (ou: python -m ziglang cc ...) */
#include "log_mask.h"
#include <stdio.h>

static int falhas = 0, total = 0;
static void caso(const char *in, const char *esperado) {
  char out[2048];
  log_mask(in, out, sizeof out);
  total++;
  if (strcmp(out, esperado)) { falhas++; printf("FALHOU\n  in : %s\n  out: %s\n  esp: %s\n", in, out, esperado); }
}
static void nao_contem(const char *in, const char *proibido) {
  char out[2048];
  log_mask(in, out, sizeof out);
  total++;
  if (strstr(out, proibido)) { falhas++; printf("VAZOU \"%s\" em: %s\n", proibido, out); }
}
int main(void) {
  caso("Iniciando download de arquivo pelo app (1/3): https://pub-c6518eb1477949a58d959f5a769703d1.r2.dev/JK-MOD/PLUGINS/VAR/jk_check_var.prx",
       "Iniciando download de arquivo pelo app (1/3): [link]/jk_check_var.prx");
  caso("Item 2/3 ja existe: https://jkpatchs.jean-rgsocd.workers.dev/f/AbCdEfGhIjKlMn0123456789abcdefgh/JK_VAR.cpk?once=29195776",
       "Item 2/3 ja existe: [link]/JK_VAR.cpk");
  caso("modulo: baixando v1.48 de https://pub-x.r2.dev/APP-PS4/MODULO/jkui.prx", "modulo: baixando v1.48 de [link]/jkui.prx");
  caso("http: /catalog -> HTTP 200", "http: /catalog -> HTTP 200");
  caso("Authorization: Bearer eyJ1IjoiYW5hIiwiZXhwIjoxfQ.c2lnbmF0dXJl", "Authorization: Bearer [oculto]");
  caso("GET /me?t=eyJabc.def&x=1", "GET /me?t=[oculto]&x=1");
  caso("conectado de 192.168.0.15 porta 9091", "conectado de [ip] porta 9091");
  caso("versao 1.51 e 10.0 e 4.16", "versao 1.51 e 10.0 e 4.16");
  caso("ID do console: a1b2c3d4e5f60718293a4b5c6d7e8f90", "ID do console: [id]");
  caso("Botoes lidos: 0x80000000", "Botoes lidos: 0x80000000");
  caso("/data/GoldHEN/AFR/CUSA18676/JK_VAR.cpk", "/data/GoldHEN/AFR/CUSA18676/JK_VAR.cpk");
  caso("Plugins: [CUSA18676] /data/GoldHEN/plugins/jk_craque.prx=false", "Plugins: [CUSA18676] /data/GoldHEN/plugins/jk_craque.prx=false");
  caso("lista: https://x/a.lst|https://y/b/c.cpk", "lista: [link]/a.lst|[link]/c.cpk");
  caso("so o servidor https://jkpatchs.com", "so o servidor [link]/");
  caso("", "");
  nao_contem("baixando https://pub-c6518eb1477949a58d959f5a769703d1.r2.dev/JK-MOD/MEGA/PLACARES/GLOBO/JKPlacar.cpk?once=15286824", "r2.dev");
  nao_contem("baixando https://pub-c6518eb1477949a58d959f5a769703d1.r2.dev/JK-MOD/MEGA/PLACARES/GLOBO/JKPlacar.cpk?once=15286824", "JK-MOD/");
  nao_contem("erro pes-patches-store https://pes-patches-store.r2.cloudflarestorage.com/x.cpk", "cloudflarestorage");
  nao_contem("JKL1|var|e4c3fe6528cb2376416b709b|Ab12Cd|4.16|1|2|27aa", "e4c3fe6528cb2376416b709b");
  nao_contem("token=abc.def.ghi enviado", "abc.def");
  nao_contem("payload em http://10.0.0.5:9090/p.bin", "10.0.0.5");
  /* linha enorme nao estoura o buffer */
  { static char big[6000]; memset(big, 'a', sizeof big - 1); char out[100]; log_mask(big, out, sizeof out); total++; if (strlen(out) != 99) { falhas++; puts("FALHOU limite do buffer"); } }
  if (falhas) printf("%d FALHA(S) em %d casos\n", falhas, total); else printf("TODOS OS %d CASOS OK\n", total);
  return falhas ? 1 : 0;
}
