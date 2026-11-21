/* Simulador da tela JK-MOD PLACARES (v1.37): abre a pasta Plugins & Mods, entra no card e salva capturas.
 * Uso: sim_placar <assets_dir> <catalog.dat> <saida_dir> <pasta_local_das_imagens_R2> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "../../src/ui.h"

static const char *LOCAL;
static int installs = 0;
static CatButton last;

static void on_install(const CatButton *b, void *user) { (void)user; installs++; last = *b; }

/* "baixa" as imagens copiando da pasta local (mesmo caminho que tera no R2 depois de PLACARES_JK/ ou COVERS/) */
static int fake_fetch(const char *url, const char *dest, void *ud) {
  (void)ud;
  const char *k = strstr(url, "PLACARES_JK/");
  char p[600];
  if (k) snprintf(p, sizeof(p), "%s/%s", LOCAL, k + 12);
  else if ((k = strstr(url, "COVERS/capa_jkmods"))) snprintf(p, sizeof(p), "/home/claude/jkmods_r2/capa_jkmods.png");
  else if ((k = strstr(url, "JKMODS/"))) snprintf(p, sizeof(p), "/home/claude/jkmods_r2/%s", k + 7);
  else return -1;
  FILE *a = fopen(p, "rb"); if (!a) return -1;
  FILE *b = fopen(dest, "wb"); if (!b) { fclose(a); return -1; }
  char buf[65536]; size_t n;
  while ((n = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, n, b);
  fclose(a); fclose(b);
  usleep(20000);
  return 0;
}

static void shot(Image *s, const char *dir, const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.png", dir, name);
  unsigned char *rgb = (unsigned char *)malloc((size_t)s->w * s->h * 3);
  for (int i = 0; i < s->w * s->h; i++) { uint32_t c = s->px[i]; rgb[3*i] = (c >> 16) & 255; rgb[3*i+1] = (c >> 8) & 255; rgb[3*i+2] = c & 255; }
  stbi_write_png(path, s->w, s->h, 3, rgb, s->w * 3);
  free(rgb);
  printf("salvo %s\n", path);
}
static void settle(UI *ui, int ms) { for (int t = 0; t < ms / 10; t++) { ui_tick(ui); usleep(10000); } }
static void frame(UI *ui, Image *s, const char *dir, const char *name) { settle(ui, 300); ui_draw(ui); shot(s, dir, name); }

int main(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "uso: sim_placar assets catalog.dat outdir local\n"); return 2; }
  LOCAL = argv[4];
  static Catalog cat;
  if (catalog_load(&cat, argv[2]) != 0) { fprintf(stderr, "catalogo invalido\n"); return 1; }
  printf("quadros=%d opcoes=%d combos=%d monte=%d extra_comum=%zu\n", cat.nplb, cat.nplb ? cat.plb[0].nopt : 0, cat.nplb ? cat.plb[0].npack : 0, cat.nplb ? cat.plb[0].monte : 0, cat.nplb ? strlen(cat.plb[0].extra) : 0);
  Image *s = img_new(1920, 1080);
  UI *ui = ui_create(s, &cat, argv[1], on_install, NULL);
  if (!ui) return 1;
  system("rm -rf /tmp/simcov && mkdir -p /tmp/simcov");
  ui_set_cover_fetcher(ui, fake_fetch, "/tmp/simcov", NULL);
  ui_skip_splash(ui);
  ui_set_user(ui, "jean");
  ui_set_plan(ui, "PLANO ATIVO  \xC2\xB7  EXPIRA 05/12/2026", 0);
  ui_set_catalog(ui, &cat);
  settle(ui, 500);
  ui_key(ui, UI_RIGHT);                                   /* card "Plugins & Mods" */
  ui_key(ui, UI_OK);                                      /* abre a pasta */
  frame(ui, s, argv[3], "01_pasta_plugins");
  ui_key(ui, UI_OK); settle(ui, 1500); frame(ui, s, argv[3], "10_placares_mega");
  ui_key(ui, UI_R2); settle(ui, 1500); frame(ui, s, argv[3], "11_narr_mega");
  ui_key(ui, UI_RIGHT); ui_key(ui, UI_RIGHT); ui_key(ui, UI_RIGHT); ui_key(ui, UI_DOWN); frame(ui, s, argv[3], "12_narr_mega_foco");
  ui_key(ui, UI_OK); printf("NARR: name=%s url=%s dest=%s size=%llu extras=%zu\n", last.name, last.url, last.dest, last.size, strlen(last.extra));
  ui_install_done(ui, 0, NULL); settle(ui,200);
  ui_key(ui, UI_R1); settle(ui, 1200); frame(ui, s, argv[3], "13_narr_brasfoot");
  ui_key(ui, UI_RIGHT); ui_key(ui, UI_OK); printf("NARR BF: name=%s url=%s dest=%s size=%llu\n", last.name, last.url, last.dest, last.size);
  ui_install_done(ui, 0, NULL);
  ui_key(ui, UI_R1); settle(ui, 1200); frame(ui, s, argv[3], "14_narr_demisan");
  ui_key(ui, UI_R2); settle(ui, 1500); frame(ui, s, argv[3], "15_ml_demisan");
  ui_key(ui, UI_DOWN); ui_key(ui, UI_RIGHT); ui_key(ui, UI_OK); printf("ML: name=%s url=%s dest=%s size=%llu\n", last.name, last.url, last.dest, last.size);
  ui_install_done(ui, 0, NULL);
  ui_key(ui, UI_R1); settle(ui, 1200); frame(ui, s, argv[3], "16_ml_mega");
  ui_key(ui, UI_R2); settle(ui, 800); frame(ui, s, argv[3], "17_volta_placares");
  ui_key(ui, UI_BACK); frame(ui, s, argv[3], "18_pasta");
  return 0;
}
