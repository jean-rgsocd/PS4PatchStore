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
  printf("  >> DOWNLOAD do video %s -> %s\n", url, dest); for (int i = 0; i <= 100; i += 25) { *pct = i; usleep(20000); }
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
  char p[300]; snprintf(p, sizeof(p), "grep -E 'craque|comemorar|pcc_v305' %s/data/GoldHEN/plugins.ini | tr -d '\\r' | sed 's/^/  [%s] /'", ROOT, tag); system(p);
  snprintf(p, sizeof(p), "python3 -c \"import sys;d=open('%s/data/GoldHEN/AFR/CUSA18676/DpFileList.bin','rb').read();n=int.from_bytes(d[4:8],'little');print('  [%s] DpFileList:',[d[16+48*k:48+48*k].split(b'\\\\0')[0].decode() for k in range(n)])\"", ROOT, tag); system(p);
  snprintf(p, sizeof(p), "sed 's/^/  [%s] estado: /' %s/data/jk/plugins.txt", tag, ROOT); system(p);
}
static void keys(UI *ui, int k, int n) { for (int i = 0; i < n; i++) ui_key(ui, k); }

static void cpx(const char *rel, int n) { char p[400]; snprintf(p, sizeof(p), "head -c %d /dev/zero > %s%s", n, ROOT, rel); if (system(p)) {} }
int main(int argc, char **argv) {
  if (argc < 7) return 2;
  ART = argv[4]; ROOT = argv[5]; JKV = argv[6];
  static Catalog cat;
  if (catalog_load(&cat, argv[2]) != 0) return 1;
  jkx_set_root(argv[5]); jkx_set_catalog(&cat); jkx_set_log(lg);
  VpPlatform vp = {0}; vp.download = fake_dl; vp.sleep_us = su; vp.log = lg; vp.tmp_path = "/tmp/sim48_video.jkv"; vp.cache_dir = "/tmp/sim48_cache";
  if (system("rm -rf /tmp/sim48_cache; mkdir -p /tmp/sim48_cache")) {}
  vp_init(&vp);
  jkx_apply(lg); ini("inicio");
  printf("--- botao do Mega baixou pcc_v305 ---\n");
  cpx("/data/GoldHEN/plugins/pcc_v305.prx", 3911216);
  jkx_apply(lg); ini("pcc baixado");
  Image *s = img_new(1920, 1080);
  UI *ui = ui_create(s, &cat, argv[1], on_install, NULL);
  if (system("rm -rf /tmp/simcov && mkdir -p /tmp/simcov")) {}
  ui_set_cover_fetcher(ui, fake_fetch, "/tmp/simcov", NULL);
  ui_skip_splash(ui); ui_set_user(ui, "jean");
  ui_set_plan(ui, "PLANO ATIVO", 0);
  ui_set_catalog(ui, &cat);
  settle(ui, 500);
  ui_key(ui, UI_RIGHT); ui_key(ui, UI_RIGHT); ui_key(ui, UI_OK); settle(ui, 800);
  ui_key(ui, UI_L2); settle(ui, 1500); frame(ui, s, argv[3], "b1_ordem");
  ui_key(ui, UI_DOWN); keys(ui, UI_RIGHT, 5); frame(ui, s, argv[3], "b2_comemoracoes");
  ui_key(ui, UI_OK); settle(ui, 200); ini("comemoracoes OFF");
  ui_key(ui, UI_OK); settle(ui, 200); ini("comemoracoes ON");
  ui_key(ui, UI_UP); keys(ui, UI_LEFT, 3); settle(ui, 3000); frame(ui, s, argv[3], "b3_dribles_video");
  ui_key(ui, UI_RIGHT); settle(ui, 300);
  ui_key(ui, UI_LEFT); settle(ui, 3000); printf("(2a vez: nao pode baixar de novo)\n");
  if (system("ls -la /tmp/sim48_cache")) {}
  vp_stop(); settle(ui, 300);
  return 0;
}
