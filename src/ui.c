#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ tema (igual ao site) */
#define C_BG         RGB(0x08, 0x0d, 0x18)
#define C_PANEL      RGB(0x11, 0x1a, 0x2b)
#define C_PANEL2     RGB(0x18, 0x24, 0x3a)
#define C_TEXT       RGB(0xf4, 0xf7, 0xff)
#define C_MUTED      RGB(0xa1, 0xae, 0xc4)
#define C_ACCENT     RGB(0x42, 0xe6, 0xa4)
#define C_BASE       RGB(0x17, 0x6b, 0x43)
#define C_BASE_EDGE  RGB(0x26, 0x96, 0x5f)
#define C_FOCUS      RGB(0xff, 0xc8, 0x57)
#define C_DARKTXT    RGB(0x06, 0x28, 0x1b)
#define C_ERR        RGB(0xff, 0x6b, 0x6b)
#define C_WHITE      RGB(0xff, 0xff, 0xff)
#define C_BLACK      RGB(0x00, 0x00, 0x00)

/* ------------------------------------------------------------------ layout (1920x1080) */
#define W            1920
#define H            1080
#define MARGIN       96
#define COVER_W      280
#define COVER_H      470
#define COVER_GAP    40
#define VISIBLE      3
#define CARDS_Y      350
#define PANEL_X      1090
#define PANEL_Y      350
#define PANEL_W      734
#define PANEL_H      550
#define BTN_H        84
#define BTN_GAP      16
#define BTN_VISIBLE  3
#define TOAST_TICKS  360

struct UI {
  Image *s;
  const Catalog *cat;
  UiInstallFn fn;
  void *user;
  Font *f, *fb;
  Image *bg;
  Image *thumb[CAT_MAX_GAMES];

  int filt[CAT_MAX_GAMES], nfilt;
  int cat_idx, sel, scroll, focus;

  int busy, dirty;
  unsigned frame;
  char toast[256];
  int toast_left, toast_kind;
  char assets[256];
};

/* ------------------------------------------------------------------ utilidades */
static void refilter(UI *ui) {
  ui->nfilt = 0;
  const char *id = ui->cat->cat[ui->cat_idx].id;
  for (int i = 0; i < ui->cat->ngame; i++) {
    if (ui->cat_idx == 0 || !strcmp(ui->cat->game[i].category, id)) ui->filt[ui->nfilt++] = i;
  }
}

static const CatGame *cur_game(UI *ui) {
  if (ui->nfilt <= 0) return NULL;
  return &ui->cat->game[ui->filt[ui->sel]];
}

/* Botao que ja vem focado: o ultimo que nao e "base" (normalmente o update); senao o primeiro. */
static int default_focus(const CatGame *g) {
  if (!g) return 0;
  for (int i = g->nbtn - 1; i >= 0; i--) if (!g->btn[i].base) return i;
  return 0;
}

static void fix_scroll(UI *ui) {
  if (ui->sel < ui->scroll) ui->scroll = ui->sel;
  if (ui->sel >= ui->scroll + VISIBLE) ui->scroll = ui->sel - VISIBLE + 1;
  if (ui->scroll < 0) ui->scroll = 0;
}

static void select_game(UI *ui, int idx) {
  ui->sel = idx;
  fix_scroll(ui);
  ui->focus = default_focus(cur_game(ui));
  ui->dirty = 1;
}

static void type_label(const char *type, char *out, int cap) {
  if (!strcmp(type, "PS4GD")) snprintf(out, cap, "JOGO");
  else if (!strcmp(type, "PS4GP")) snprintf(out, cap, "PATCH");
  else if (!strcmp(type, "PS4AC")) snprintf(out, cap, "DLC");
  else out[0] = 0;
}

static void info_text(const CatButton *b, char *out, int cap) {
  char t[16] = "";
  type_label(b->type, t, sizeof(t));
  char sz[32] = "";
  if (b->size) {
    double gib = (double)b->size / 1073741824.0;
    snprintf(sz, sizeof(sz), "%.1f GB", gib);
    for (char *p = sz; *p; p++) if (*p == '.') *p = ',';
  }
  if (t[0] && sz[0]) snprintf(out, cap, "%s  \xE2\x80\xA2  %s", t, sz);   /* bullet */
  else snprintf(out, cap, "%s%s", t, sz);
}

/* ------------------------------------------------------------------ criacao */
static Image *build_bg(UI *ui) {
  Image *bg = img_new(W, H);
  if (!bg) return NULL;
  gfx_vgradient(bg, 0, 0, W, H, RGB(0x0d, 0x18, 0x30), C_BG);
  char path[320];
  snprintf(path, sizeof(path), "%s/images/stadium_bg.jpg", ui->assets);
  Image *pic = img_load(path);
  if (pic) {
    Image *sc = img_scale_cover(pic, W, H);
    if (sc) {
      gfx_blit(bg, sc, 0, 0, 0, 255);
      img_free(sc);
      /* escurece: bem forte no topo (legibilidade) e some no fim */
      gfx_vfade(bg, 0, 0, W, 780, C_BG, 205, 252);
      gfx_fill(bg, 0, 780, W, H - 780, C_BG, 255);
    }
    img_free(pic);
  }
  return bg;
}

UI *ui_create(Image *surface, const Catalog *cat, const char *assets_dir, UiInstallFn fn, void *user) {
  UI *ui = (UI *)calloc(1, sizeof(UI));
  if (!ui) return NULL;
  ui->s = surface;
  ui->cat = cat;
  ui->fn = fn;
  ui->user = user;
  snprintf(ui->assets, sizeof(ui->assets), "%s", assets_dir);

  char path[320];
  snprintf(path, sizeof(path), "%s/fonts/DejaVuSans.ttf", assets_dir);
  ui->f = font_load(path);
  snprintf(path, sizeof(path), "%s/fonts/DejaVuSans-Bold.ttf", assets_dir);
  ui->fb = font_load(path);
  if (!ui->f) ui->f = ui->fb;
  if (!ui->fb) ui->fb = ui->f;

  ui->bg = build_bg(ui);

  for (int i = 0; i < cat->ngame; i++) {
    if (!cat->game[i].cover[0]) continue;
    snprintf(path, sizeof(path), "%s/images/%s", assets_dir, cat->game[i].cover);
    Image *raw = img_load(path);
    if (raw) {
      ui->thumb[i] = img_scale_cover(raw, COVER_W, COVER_H);
      img_free(raw);
    }
  }

  refilter(ui);
  select_game(ui, 0);
  return ui;
}

/* ------------------------------------------------------------------ entrada */
void ui_toast(UI *ui, const char *msg, int kind) {
  snprintf(ui->toast, sizeof(ui->toast), "%s", msg);
  ui->toast_kind = kind;
  ui->toast_left = TOAST_TICKS;
  ui->dirty = 1;
}

void ui_set_busy(UI *ui, int busy) {
  ui->busy = busy;
  ui->dirty = 1;
}

static void trigger_install(UI *ui) {
  const CatGame *g = cur_game(ui);
  if (!g || g->nbtn == 0) return;
  const CatButton *b = &g->btn[ui->focus];
  if (ui->busy) { ui_toast(ui, "Já existe um pedido em andamento. Aguarde.", UI_TOAST_INFO); return; }
  if (!b->url[0]) { ui_toast(ui, "URL do pacote não configurada neste botão.", UI_TOAST_ERR); return; }
  if (!b->content_id[0]) {
    ui_toast(ui, "Este botão está sem ID do PKG (data-content-id).", UI_TOAST_ERR);
    return;
  }
  ui_toast(ui, "Enviando pedido ao instalador nativo...", UI_TOAST_INFO);
  ui->busy = 1;
  if (ui->fn) ui->fn(b, ui->user);
}

void ui_key(UI *ui, int key) {
  const CatGame *g = cur_game(ui);
  switch (key) {
    case UI_LEFT:
      if (ui->sel > 0) select_game(ui, ui->sel - 1);
      break;
    case UI_RIGHT:
      if (ui->sel + 1 < ui->nfilt) select_game(ui, ui->sel + 1);
      break;
    case UI_UP:
      if (g && ui->focus > 0) { ui->focus--; ui->dirty = 1; }
      break;
    case UI_DOWN:
      if (g && ui->focus + 1 < g->nbtn) { ui->focus++; ui->dirty = 1; }
      break;
    case UI_L1:
    case UI_R1: {
      int n = ui->cat->ncat;
      ui->cat_idx = (ui->cat_idx + (key == UI_R1 ? 1 : n - 1)) % n;
      refilter(ui);
      ui->scroll = 0;
      select_game(ui, 0);
      break;
    }
    case UI_BACK:
      if (ui->sel != 0) { ui->scroll = 0; select_game(ui, 0); }
      break;
    case UI_OK:
      trigger_install(ui);
      break;
    default:
      break;
  }
}

void ui_tick(UI *ui) {
  ui->frame++;
  if (ui->busy && ui->frame % 6 == 0) ui->dirty = 1;          /* animacao do spinner */
  if (ui->toast_left > 0 && !ui->busy) {
    if (--ui->toast_left == 0) ui->dirty = 1;
  }
}

int ui_take_dirty(UI *ui) {
  int d = ui->dirty;
  ui->dirty = 0;
  return d;
}

/* ------------------------------------------------------------------ desenho */
static int T(UI *ui, int bold, int px, int x, int base, const char *s, uint32_t rgb, int alpha, int spacing) {
  return text_draw(ui->s, bold ? ui->fb : ui->f, px, x, base, s, rgb, alpha, spacing);
}
static int TW(UI *ui, int bold, int px, const char *s, int spacing) {
  return text_width(bold ? ui->fb : ui->f, px, s, spacing);
}

static void draw_topbar(UI *ui) {
  Image *s = ui->s;
  gfx_fill(s, 0, 0, W, 110, C_BG, 120);
  gfx_fill(s, 0, 109, W, 1, C_WHITE, 26);

  gfx_rrect(s, MARGIN, 28, 84, 54, 12, C_ACCENT, 255, 3);
  text_draw_in(s, ui->fb, 26, MARGIN, 84, 64, "PS4", C_ACCENT, 255, 0, 1);
  int x = MARGIN + 84 + 22;
  x += T(ui, 1, 28, x, 66, "PATCH ", C_TEXT, 255, 3) + 3 + 8;
  T(ui, 1, 28, x, 66, "STORE", C_ACCENT, 255, 3);

  const char *st = "CATÁLOGO LOCAL";
  int w = TW(ui, 1, 20, st, 3);
  T(ui, 1, 20, W - MARGIN - w, 62, st, C_MUTED, 255, 3);
  gfx_circle(s, W - MARGIN - w - 22, 55, 7, C_ACCENT, 255);
}

static void draw_heading(UI *ui) {
  T(ui, 1, 22, MARGIN, 168, "ENCONTRE SEU JOGO", C_ACCENT, 255, 4);
  T(ui, 1, 54, MARGIN, 228, "Nosso catálogo", C_TEXT, 255, 0);
  char cnt[48];
  if (ui->nfilt > VISIBLE)                       /* varias paginas: mostra a posicao ("2 de 7 coleções") */
    snprintf(cnt, sizeof(cnt), "%d de %d coleções", ui->sel + 1, ui->nfilt);
  else
    snprintf(cnt, sizeof(cnt), "%d %s", ui->nfilt, ui->nfilt == 1 ? "coleção" : "coleções");
  int w = TW(ui, 0, 26, cnt, 0);
  T(ui, 0, 26, W - MARGIN - w, 226, cnt, C_MUTED, 255, 0);
}

static void draw_chips(UI *ui) {
  Image *s = ui->s;
  int y = 262, h = 52;
  int x = MARGIN;
  /* dica L1 */
  gfx_rrect(s, x, y + 6, 56, 40, 10, C_PANEL2, 255, 0);
  text_draw_in(s, ui->fb, 20, x, 56, y + 33, "L1", C_MUTED, 255, 0, 1);
  x += 56 + 18;
  for (int i = 0; i < ui->cat->ncat; i++) {
    const char *lab = ui->cat->cat[i].label;
    int tw = TW(ui, 1, 22, lab, 0);
    int w = tw + 52;
    if (i == ui->cat_idx) {
      gfx_rrect(s, x, y, w, h, h / 2, C_ACCENT, 255, 0);
      text_draw_in(s, ui->fb, 22, x, w, y + 34, lab, C_DARKTXT, 255, 0, 1);
    } else {
      gfx_rrect(s, x, y, w, h, h / 2, C_PANEL2, 230, 0);
      gfx_rrect(s, x, y, w, h, h / 2, C_WHITE, 26, 2);
      text_draw_in(s, ui->fb, 22, x, w, y + 34, lab, C_MUTED, 255, 0, 1);
    }
    x += w + 14;
  }
  x += 4;
  gfx_rrect(s, x, y + 6, 56, 40, 10, C_PANEL2, 255, 0);
  text_draw_in(s, ui->fb, 20, x, 56, y + 33, "R1", C_MUTED, 255, 0, 1);
}

static void draw_cover_placeholder(UI *ui, int x, int y, const char *title) {
  Image *s = ui->s;
  gfx_rrect(s, x, y, COVER_W, COVER_H, 18, C_PANEL2, 255, 0);
  gfx_rrect(s, x, y, COVER_W, COVER_H, 18, C_WHITE, 26, 2);
  char ini[8] = {0};
  ini[0] = title[0] ? title[0] : '?';
  text_draw_in(s, ui->fb, 120, x, COVER_W, y + COVER_H / 2 + 40, ini, C_MUTED, 255, 0, 1);
}

static void draw_cards(UI *ui) {
  Image *s = ui->s;
  if (ui->nfilt == 0) {
    text_draw_in(s, ui->f, 30, MARGIN, 940, CARDS_Y + 240, "Nenhuma coleção encontrada nesta categoria.",
                 C_MUTED, 255, 0, 1);
    return;
  }
  for (int v = 0; v < VISIBLE; v++) {
    int idx = ui->scroll + v;
    if (idx >= ui->nfilt) break;
    const CatGame *g = &ui->cat->game[ui->filt[idx]];
    int x = MARGIN + v * (COVER_W + COVER_GAP), y = CARDS_Y;
    int selected = (idx == ui->sel);

    if (selected) gfx_rrect(s, x - 16, y - 16, COVER_W + 32, COVER_H + 32, 30, C_ACCENT, 20, 0);   /* brilho */
    gfx_rrect(s, x + 6, y + 14, COVER_W, COVER_H, 18, C_BLACK, 90, 0);                              /* sombra */

    Image *t = ui->thumb[ui->filt[idx]];
    if (t) gfx_blit(s, t, x, y, 18, 255);
    else draw_cover_placeholder(ui, x, y, g->title);
    if (!selected) gfx_rrect(s, x, y, COVER_W, COVER_H, 18, C_BG, 120, 0);                          /* escurece */
    if (selected) gfx_rrect(s, x - 8, y - 8, COVER_W + 16, COVER_H + 16, 24, C_ACCENT, 255, 5);

    /* etiqueta (PATCH / COLEÇÃO ...) */
    if (g->tag[0]) {
      int tw = TW(ui, 1, 17, g->tag, 2);
      gfx_rrect(s, x + 14, y + 14, tw + 28, 34, 17, C_BG, 215, 0);
      text_draw_in(s, ui->fb, 17, x + 14, tw + 28, y + 38, g->tag, C_ACCENT, 255, 2, 1);
    }

    char buf[128];
    text_fit(ui->fb, 25, g->title, COVER_W, buf, sizeof(buf));
    T(ui, 1, 25, x, y + COVER_H + 44, buf, selected ? C_TEXT : C_MUTED, 255, 0);
    text_fit(ui->f, 19, g->kicker, COVER_W, buf, sizeof(buf));
    T(ui, 0, 19, x, y + COVER_H + 74, buf, C_MUTED, selected ? 255 : 170, 0);
  }
  /* setas quando ha mais cards fora da janela */
  if (ui->scroll > 0) T(ui, 1, 30, MARGIN - 46, CARDS_Y + COVER_H / 2 + 10, "\xE2\x97\x80", C_ACCENT, 255, 0);
  if (ui->scroll + VISIBLE < ui->nfilt)
    T(ui, 1, 30, MARGIN + VISIBLE * COVER_W + (VISIBLE - 1) * COVER_GAP + 16, CARDS_Y + COVER_H / 2 + 10,
      "\xE2\x96\xB6", C_ACCENT, 255, 0);
}

static void draw_button(UI *ui, const CatButton *b, int x, int y, int w, int focused) {
  Image *s = ui->s;
  int r = 20;
  if (b->base) {
    gfx_rrect(s, x, y, w, BTN_H, r, C_BASE, 255, 0);
    gfx_rrect(s, x, y, w, BTN_H, r, C_BASE_EDGE, 255, 2);
  } else {
    gfx_rrect(s, x, y, w, BTN_H, r, C_ACCENT, 255, 0);
  }
  uint32_t fg = b->base ? C_WHITE : C_DARKTXT;
  char info[48];
  info_text(b, info, sizeof(info));
  int iw = info[0] ? TW(ui, 0, 20, info, 0) : 0;
  int arrow_w = 56;
  int avail = w - 40 - arrow_w - (iw ? iw + 24 : 0);
  char lab[128];
  text_fit(ui->fb, 27, b->label, avail, lab, sizeof(lab));
  T(ui, 1, 27, x + 32, y + BTN_H / 2 + 10, lab, fg, 255, 0);
  if (info[0]) T(ui, 0, 20, x + w - arrow_w - 12 - iw, y + BTN_H / 2 + 7, info, fg, 190, 0);
  text_draw_in(s, ui->fb, 30, x + w - arrow_w - 4, arrow_w, y + BTN_H / 2 + 11, "\xE2\x86\x93", fg, 255, 0, 1);
  if (!focused) gfx_rrect(s, x, y, w, BTN_H, r, C_BG, 70, 0);
  else gfx_rrect(s, x - 8, y - 8, w + 16, BTN_H + 16, r + 8, C_FOCUS, 255, 4);
}

static void draw_panel(UI *ui) {
  Image *s = ui->s;
  gfx_rrect(s, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 26, C_PANEL, 240, 0);
  gfx_rrect(s, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 26, C_WHITE, 26, 2);
  const CatGame *g = cur_game(ui);
  if (!g) return;

  int px = PANEL_X + 44, iw = PANEL_W - 88;
  T(ui, 1, 22, px, PANEL_Y + 66, g->kicker, C_ACCENT, 255, 3);
  int tl = text_wrap(s, ui->fb, 46, px, PANEL_Y + 128, iw, 56, 2, g->title, C_TEXT, 255);
  if (tl < 1) tl = 1;
  int desc_y = PANEL_Y + 128 + (tl - 1) * 56 + 54;

  int nb = g->nbtn < BTN_VISIBLE ? g->nbtn : BTN_VISIBLE;
  int bottom = PANEL_Y + PANEL_H - 44;
  int btn_top = bottom - (nb > 0 ? nb * BTN_H + (nb - 1) * BTN_GAP : 0);
  int lines = (btn_top - 28 - desc_y) / 36 + 1;
  if (lines < 1) lines = 1;
  if (lines > 5) lines = 5;
  text_wrap(s, ui->f, 25, px, desc_y, iw, 36, lines, g->desc, C_MUTED, 255);

  if (g->nbtn == 0) {
    text_draw_in(s, ui->f, 24, px, iw, bottom - 20, "Nenhum download disponível ainda.", C_MUTED, 255, 0, 0);
    return;
  }
  /* janela de botoes que acompanha o foco */
  int first = 0;
  if (ui->focus >= BTN_VISIBLE) first = ui->focus - BTN_VISIBLE + 1;
  for (int i = 0; i < nb; i++) {
    int bi = first + i;
    draw_button(ui, &g->btn[bi], px, btn_top + i * (BTN_H + BTN_GAP), iw, bi == ui->focus);
  }
  if (first > 0) T(ui, 1, 22, PANEL_X + PANEL_W - 44, btn_top - 10, "\xE2\x96\xB2", C_MUTED, 255, 0);
  if (first + nb < g->nbtn) T(ui, 1, 22, PANEL_X + PANEL_W - 44, bottom + 32, "\xE2\x96\xBC", C_MUTED, 255, 0);
}

static void draw_spinner(UI *ui, int cx, int cy) {
  int head = (int)(ui->frame / 6) % 8;
  static const int dx[8] = {0, 7, 10, 7, 0, -7, -10, -7};
  static const int dy[8] = {-10, -7, 0, 7, 10, 7, 0, -7};
  for (int i = 0; i < 8; i++) {
    int age = (head - i + 8) % 8;
    gfx_circle(ui->s, cx + dx[i], cy + dy[i], 3, C_ACCENT, 255 - age * 30);
  }
}

static void draw_message_line(UI *ui) {
  Image *s = ui->s;
  if (ui->toast_left > 0 || ui->busy) {
    uint32_t edge = ui->toast_kind == UI_TOAST_ERR ? C_ERR : (ui->toast_kind == UI_TOAST_OK ? C_ACCENT : C_FOCUS);
    char buf[256];
    text_fit(ui->f, 26, ui->toast, W - 2 * MARGIN - 160, buf, sizeof(buf));
    int tw = TW(ui, 0, 26, buf, 0);
    int pad = 34, extra = ui->busy ? 44 : 0;
    int w = tw + pad * 2 + extra, h = 62;
    int x = (W - w) / 2, y = 912;
    gfx_rrect(s, x, y, w, h, h / 2, C_PANEL2, 250, 0);
    gfx_rrect(s, x, y, w, h, h / 2, edge, 255, 3);
    if (ui->busy) draw_spinner(ui, x + pad + 6, y + h / 2);
    T(ui, 0, 26, x + pad + extra, y + h / 2 + 9, buf, C_TEXT, 255, 0);
    return;
  }
  text_wrap(s, ui->f, 20, MARGIN, 940, W - 2 * MARGIN, 30, 2,
            "Antes de instalar: confira a compatibilidade e a versão de cada arquivo. O jogo base precisa estar "
            "instalado no console antes do update. A instalação é solicitada pelo aplicativo nativo do PS4; "
            "acompanhe em Notificações.",
            C_MUTED, 200);
}

static int hint(UI *ui, int x, const char *key, const char *label) {
  int base = 1038;
  int kw = TW(ui, 1, 20, key, 0) + 26;
  gfx_rrect(ui->s, x, base - 30, kw, 40, 10, C_PANEL2, 255, 0);
  text_draw_in(ui->s, ui->fb, 20, x, kw, base - 2, key, C_TEXT, 255, 0, 1);
  int lw = T(ui, 0, 22, x + kw + 12, base - 1, label, C_MUTED, 255, 0);
  return x + kw + 12 + lw + 44;
}

static void draw_footer(UI *ui) {
  gfx_fill(ui->s, 0, 990, W, 1, C_WHITE, 26);
  int x = MARGIN;
  x = hint(ui, x, "\xE2\x97\x80 \xE2\x96\xB6", "Coleções");
  x = hint(ui, x, "\xE2\x96\xB2 \xE2\x96\xBC", "Downloads");
  x = hint(ui, x, "X", "Instalar");
  x = hint(ui, x, "L1  R1", "Categorias");
  x = hint(ui, x, "O", "Voltar ao início");
  const char *v = "PS4 Patch Store";
  int w = TW(ui, 0, 20, v, 1);
  T(ui, 0, 20, W - MARGIN - w, 1036, v, C_MUTED, 150, 1);
}

void ui_draw(UI *ui) {
  if (ui->bg) gfx_blit_surface(ui->s, ui->bg);
  else gfx_fill(ui->s, 0, 0, W, H, C_BG, 255);
  draw_topbar(ui);
  draw_heading(ui);
  draw_chips(ui);
  draw_cards(ui);
  draw_panel(ui);
  draw_message_line(ui);
  draw_footer(ui);
}
