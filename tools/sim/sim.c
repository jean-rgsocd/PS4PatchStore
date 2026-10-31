/* Simulador no PC: roda a mesma UI do app e salva capturas de tela em PNG.
 * Uso: sim <assets_dir> <catalog.dat> <saida_dir> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "../../src/ui.h"

static int installs = 0;
static char last_name[200];

static void on_install(const CatButton *b, void *user) {
  (void)user;
  installs++;
  snprintf(last_name, sizeof(last_name), "%s|%s|%s", b->name, b->type, b->content_id);
}

static void shot(Image *s, const char *dir, const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.png", dir, name);
  unsigned char *rgb = (unsigned char *)malloc((size_t)s->w * s->h * 3);
  for (int i = 0; i < s->w * s->h; i++) {
    uint32_t c = s->px[i];
    rgb[3 * i] = (c >> 16) & 255; rgb[3 * i + 1] = (c >> 8) & 255; rgb[3 * i + 2] = c & 255;
  }
  stbi_write_png(path, s->w, s->h, 3, rgb, s->w * 3);
  free(rgb);
  printf("salvo %s\n", path);
}

static void frame(UI *ui, Image *s, const char *dir, const char *name) {
  ui_draw(ui);
  shot(s, dir, name);
}

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "uso: sim assets catalog.dat outdir\n"); return 2; }
  static Catalog cat;
  if (catalog_load(&cat, argv[2]) != 0) { fprintf(stderr, "catalogo invalido\n"); return 1; }
  Image *s = img_new(1920, 1080);
  UI *ui = ui_create(s, &cat, argv[1], on_install, NULL);
  if (!ui) return 1;

  frame(ui, s, argv[3], "01_inicio");
  ui_key(ui, UI_RIGHT);  frame(ui, s, argv[3], "02_segundo_card");
  ui_key(ui, UI_UP);     frame(ui, s, argv[3], "03_foco_base");
  ui_key(ui, UI_DOWN);
  ui_key(ui, UI_OK);     frame(ui, s, argv[3], "04_instalando");
  ui_key(ui, UI_OK);     /* busy: deve avisar e nao disparar de novo */
  for (int i = 0; i < 6; i++) ui_tick(ui);
  frame(ui, s, argv[3], "05_busy_aviso");
  ui_set_busy(ui, 0);
  ui_toast(ui, "Pedido aceito! Acompanhe nas Notificações do PS4.", UI_TOAST_OK);
  frame(ui, s, argv[3], "06_ok");
  ui_toast(ui, "Payload não enviado. O BinLoader do GoldHEN está ligado?", UI_TOAST_ERR);
  frame(ui, s, argv[3], "07_erro");
  for (int i = 0; i < 400; i++) ui_tick(ui);
  ui_key(ui, UI_R1);     frame(ui, s, argv[3], "08_categoria_patches");
  ui_key(ui, UI_R1); ui_key(ui, UI_R1); ui_key(ui, UI_R1);
  ui_key(ui, UI_L1);     frame(ui, s, argv[3], "09_categoria_copa");
  printf("instalacoes disparadas: %d (esperado 1) ultima=%s\n", installs, last_name);
  return installs == 1 ? 0 : 1;
}
