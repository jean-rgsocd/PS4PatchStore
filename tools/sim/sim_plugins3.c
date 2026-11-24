/* v1.47: ATIVADO/DESATIVADO + video no card. Uso: sim_plugins3 <assets> <catalog.dat> <saida> <pasta_artes> <raiz_fake> <jkv> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "../../src/ui.h"
#include "../../src/jkplug.h"
#include "../../src/vplayer.h"

static const char *ART, *JKV, *ROOT;
static CatButton last; static int ninst = 0;
static void on_install(const CatButton *b, void *user) { (void)user; last = *b; ninst++; }
static int cp(const char *a_, const char *b_) {
  FILE *a = fopen(a_, "rb"); if (!a) return -1;
  FILE *b = fopen(b_, "wb"); char buf[65536]; size_t n;
  while ((n = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, n, b);
  fclose(a); fclose(b); return 0;
}
static int fake_fetch(const char *url, const char *dest, void *ud) {
  (void)ud; const char *k = strstr(url, "/PLUG/"); char p[600];
  if (k) { snprintf(p, sizeof(p), "%s/%s", ART, k + 6); char *q = strchr(p, '?'); if (q) *q = 0; }
  else return -1;
  return cp(p, dest);
}
static int fake_dl(const char *url, const char *dest, volatile int *pct, volatile int *cancel, char *err, int cap) {
  (void)url; (void)cancel; (void)err; (void)cap;
  for (int i = 0; i <= 100; i += 25) { *pct = i; usleep(20000); }
  return cp(JKV, dest);
}
static void su(unsigned us) { usleep(us); }
static void shot(Image *s, const char *dir, const char *name) {
  char path[512]; snprintf(path, sizeof(path), "%s/%s.png", dir, name);
  unsigned char *rgb = (unsigned char *)malloc((size_t)s->w * s->h * 3);
  for (int i = 0; i < s->w * s->h; i++) { uint32_t c = s->px[i]; rgb[3*i] = (c >> 16) & 255; rgb[3*i+1] = (c >> 8) & 255; rgb[3*i+2] = c & 255; }
  stbi_write_png(path, s->w, s->h, 3, rgb, s->w * 3); free(rgb); printf("salvo %s\n", path);
}
static void settle(UI *ui, int ms) { for (int t = 0; t < ms / 16; t++) { ui_tick(ui); usleep(16000); } }
static void frame(UI *ui, Image *s, const char *dir, const char *name) { settle(ui, 300); ui_draw(ui); shot(s, dir, name); }
static void lg(const char *m) { printf("  log: %s\n", m); }
static void ini(const char *tag) {
  char p[300]; snprintf(p, sizeof(p), "grep -E 'craque|orcamento|comemorar|menu_ml' %s/data/GoldHEN/plugins.ini | tr -d '\\r' | sed 's/^/  [%s] /'", ROOT, tag); system(p);
  snprintf(p, sizeof(p), "python3 -c \"import sys;d=open('%s/data/GoldHEN/AFR/CUSA18676/DpFileList.bin','rb').read();n=int.from_bytes(d[4:8],'little');print('  [%s] DpFileList:',[d[16+48*k:48+48*k].split(b'\\\\0')[0].decode() for k in range(n)])\"", ROOT, tag); system(p);
  snprintf(p, sizeof(p), "sed 's/^/  [%s] estado: /' %s/data/jk/plugins.txt", tag, ROOT); system(p);
}
static void keys(UI *ui, int k, int n) { for (int i = 0; i < n; i++) ui_key(ui, k); }

int main(int argc, char **argv) {
  if (argc < 7) return 2;
  ART = argv[4]; ROOT = argv[5]; JKV = argv[6];
  static Catalog cat;
  if (catalog_load(&cat, argv[2]) != 0) return 1;
  jkx_set_root(argv[5]); jkx_set_catalog(&cat);
  VpPlatform vp = {0}; vp.download = fake_dl; vp.sleep_us = su; vp.log = lg; vp.tmp_path = "/tmp/sim47_video.jkv";
  vp_init(&vp);
  jkx_apply(lg); ini("inicio");
  Image *s = img_new(1920, 1080);
  UI *ui = ui_create(s, &cat, argv[1], on_install, NULL);
  system("rm -rf /tmp/simcov && mkdir -p /tmp/simcov");
  ui_set_cover_fetcher(ui, fake_fetch, "/tmp/simcov", NULL);
  ui_skip_splash(ui); ui_set_user(ui, "jean");
  ui_set_plan(ui, "PLANO ATIVO", 0);
  ui_set_catalog(ui, &cat);
  settle(ui, 500);
  ui_key(ui, UI_RIGHT); ui_key(ui, UI_RIGHT); ui_key(ui, UI_OK); settle(ui, 800);
  ui_key(ui, UI_L2); settle(ui, 1500);
  ui_key(ui, UI_RIGHT); frame(ui, s, argv[3], "a1_craque_ativado");
  ui_key(ui, UI_OK); settle(ui, 200); frame(ui, s, argv[3], "a2_craque_desativado"); ini("craque OFF");
  ui_key(ui, UI_OK); settle(ui, 200); ini("craque ON");
  /* orcamento: linha 2, coluna 4 (indice 10) */
  ui_key(ui, UI_DOWN); keys(ui, UI_RIGHT, 2); frame(ui, s, argv[3], "a3_orcamento_sem_tela");
  ui_key(ui, UI_OK); printf("ORCAMENTO X: action=%s name=%s url=%s extra=%s\n", last.action, last.name, last.url, last.extra);
  { char p[300]; snprintf(p, sizeof(p), "head -c 54976 /dev/zero > %s/data/GoldHEN/plugins/pes21_orcamento_tela.prx", ROOT); system(p); }
  jkx_mark_install(last.name); jkx_apply(lg); ui_install_done(ui, 1, "Download concluído"); settle(ui, 300);
  frame(ui, s, argv[3], "a4_orcamento_ativado"); ini("orc baixado");
  ui_key(ui, UI_OK); settle(ui, 200); frame(ui, s, argv[3], "a5_orcamento_desativado"); ini("orc OFF");
  { char p[400]; snprintf(p, sizeof(p), "sed -i 's/=false/=true/' %s/data/GoldHEN/plugins.ini", ROOT); system(p); }
  printf("--- botao do placar baixou o plugins.ini do R2 (tudo true) ---\n");
  jkx_apply(lg); ini("depois do R2");
  /* dribles: indice 12 */
  keys(ui, UI_RIGHT, 2); settle(ui, 300); frame(ui, s, argv[3], "a6_dribles_foco");
  settle(ui, 14000); frame(ui, s, argv[3], "a7_dribles_video_card");
  ui_key(ui, UI_OK); printf("DRIBLES X: action=%s extra=%s url=%s dest=%s\n", last.action, last.extra, last.url, last.dest);
  ui_install_done(ui, 1, "ok"); settle(ui, 300);
  ui_key(ui, UI_TRIANGLE); settle(ui, 1000); frame(ui, s, argv[3], "a8_dribles_tela_cheia");
  ui_key(ui, UI_BACK); settle(ui, 500); frame(ui, s, argv[3], "a9_fechou"); settle(ui, 4000); frame(ui, s, argv[3], "a9b_card_voltou");
  ui_key(ui, UI_LEFT); settle(ui, 300); frame(ui, s, argv[3], "a10_saiu_do_card");
  vp_stop(); settle(ui, 300);
  return 0;
}
