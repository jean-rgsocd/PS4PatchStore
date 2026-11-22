/* Simulador da aba PLUGINS (v1.43). Uso: sim_plugins <assets> <catalog.dat> <saida> <pasta_artes> <raiz_fake> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "../../src/ui.h"
#include "../../src/jkplug.h"

static const char *ART;
static CatButton last; static int ninst = 0;
static void on_install(const CatButton *b, void *user) { (void)user; last = *b; ninst++; }
static int fake_fetch(const char *url, const char *dest, void *ud) {
  (void)ud; const char *k = strstr(url, "/PLUG/"); char p[600];
  if (k) { snprintf(p, sizeof(p), "%s/%s", ART, k + 6); char *qq = strchr(p, '?'); if (qq) *qq = 0; }
  else if ((k = strstr(url, "PLACARES_MUN/IMG/"))) snprintf(p, sizeof(p), "/home/claude/mun/IMG/%s", k + 17);
  else if ((k = strstr(url, "PLACARES_JK/"))) snprintf(p, sizeof(p), "/home/claude/build_pl/r2up/%s", k + 12);
  else if ((k = strstr(url, "JKMODS/"))) snprintf(p, sizeof(p), "/home/claude/jkmods_r2/%s", k + 7);
  else if ((k = strstr(url, "COVERS/capa_jkmods"))) snprintf(p, sizeof(p), "/home/claude/jkmods_r2/capa_jkmods.png");
  else return -1;
  FILE *a = fopen(p, "rb"); if (!a) return -1;
  FILE *b = fopen(dest, "wb"); char buf[65536]; size_t n;
  while ((n = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, n, b);
  fclose(a); fclose(b); return 0;
}
static void shot(Image *s, const char *dir, const char *name) {
  char path[512]; snprintf(path, sizeof(path), "%s/%s.png", dir, name);
  unsigned char *rgb = (unsigned char *)malloc((size_t)s->w * s->h * 3);
  for (int i = 0; i < s->w * s->h; i++) { uint32_t c = s->px[i]; rgb[3*i] = (c >> 16) & 255; rgb[3*i+1] = (c >> 8) & 255; rgb[3*i+2] = c & 255; }
  stbi_write_png(path, s->w, s->h, 3, rgb, s->w * 3); free(rgb); printf("salvo %s\n", path);
}
static void settle(UI *ui, int ms) { for (int t = 0; t < ms / 10; t++) { ui_tick(ui); usleep(10000); } }
static void frame(UI *ui, Image *s, const char *dir, const char *name) { settle(ui, 300); ui_draw(ui); shot(s, dir, name); }
static void lg(const char *m) { printf("  log: %s\n", m); }

int main(int argc, char **argv) {
  if (argc < 6) return 2;
  ART = argv[4];
  static Catalog cat;
  if (catalog_load(&cat, argv[2]) != 0) return 1;
  jkx_set_root(argv[5]); jkx_set_catalog(&cat);
  jkx_note_download("/data/GoldHEN/plugins/pes21_fin_vivas_mega.prx");
  jkx_apply(lg);
  Image *s = img_new(1920, 1080);
  UI *ui = ui_create(s, &cat, argv[1], on_install, NULL);
  system("rm -rf /tmp/simcov && mkdir -p /tmp/simcov");
  ui_set_cover_fetcher(ui, fake_fetch, "/tmp/simcov", NULL);
  ui_skip_splash(ui); ui_set_user(ui, "jean");
  ui_set_plan(ui, "PLANO ATIVO", 0);
  ui_set_catalog(ui, &cat);
  settle(ui, 500);
  ui_key(ui, UI_RIGHT); ui_key(ui, UI_RIGHT); ui_key(ui, UI_OK); settle(ui, 800);
  ui_key(ui, UI_L2); settle(ui, 1500); frame(ui, s, argv[3], "q1_plugins_b0");
  ui_key(ui, UI_OK); settle(ui, 100); frame(ui, s, argv[3], "q2_fixo_x");
  ui_key(ui, UI_DOWN); ui_key(ui, UI_DOWN); frame(ui, s, argv[3], "q3_rolou");
  ui_key(ui, UI_R1); settle(ui, 1200); frame(ui, s, argv[3], "q4_b1");
  ui_key(ui, UI_R1); settle(ui, 1200); frame(ui, s, argv[3], "q5_b2");
  return 0;
}
