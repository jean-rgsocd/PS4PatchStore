/* ------------------------------------------------------------------ v1.37: tela JK-MOD PLACARES
 * Card com tag PLACARGRID. Os dados vem das linhas PLB/PLO/PLP do catalogo (ver catalog.h).
 * Grade 3x2: cada card = 1 combo (Serie A, Serie B, Copa do Brasil, Libertadores) com a imagem do placar e o logo.
 * O ultimo card e o "Monte o seu": escolhe 1 placar por competicao.
 * Controle: direcional move, X baixa / monta, O volta, L1/R1 troca o patch (Demisan / Mega...).
 * Incluido no ui.c (usa a struct UI e as funcoes de desenho de la). */

#define PL_X0   80
#define PL_Y0   130
#define PL_CW   568
#define PL_CH   422
#define PL_GX   28
#define PL_GY   16
#define PL_PAD  14
#define PL_IW   468            /* caixa da imagem do placar no card */
#define PL_IH   64
#define PL_LW   64             /* caixa do logo no card */
#define PL_RS   85             /* distancia entre as linhas de competicao no card */
#define PL_EIW  900            /* editor (Monte o seu) */
#define PL_EIH  110
#define PL_ELW  116
#define PL_MAXU (CAT_MAX_PLO * 2)
#define PL_PER  6              /* cards por pagina */

#define C_DEEP  RGB(0x0b, 0x12, 0x22)
#define C_EDGE  RGB(0x22, 0x30, 0x4b)

static const char *PL_COMP[4] = { "BRASILEIRÃO SÉRIE A", "BRASILEIRÃO SÉRIE B", "COPA DO BRASIL", "LIBERTADORES" };
static const char *PL_FILE[4] = { "A", "B", "CDB", "LIB" };

static int is_plgrid(const CatGame *g) { return g && !strcmp(g->tag, "PLACARGRID"); }
static int pl_active(UI *ui) { return ui->mode == MODE_CATALOG && ui->detail && is_plgrid(cur_game(ui)); }

static const CatPlBoard *pl_board_cur(UI *ui) {
  if (!ui->cat || ui->cat->nplb <= 0) return NULL;
  if (ui->pl_b < 0 || ui->pl_b >= ui->cat->nplb) ui->pl_b = 0;
  return &ui->cat->plb[ui->pl_b];
}
static int pl_ncards(const CatPlBoard *b) { return b ? b->npack + (b->monte ? 1 : 0) : 0; }

/* ---- imagens (baixadas em segundo plano, guardadas na mesma pasta das capas) */
static pthread_mutex_t pl_mu = PTHREAD_MUTEX_INITIALIZER;
static int plr_gen = 0, plr_pending = 0;
static Image *plr_im[PL_MAXU][2];
static int plr_rdy[PL_MAXU];

typedef struct { UI *ui; int gen, n; char url[PL_MAXU][256]; int logo[PL_MAXU]; } PlJobs;

static Image *pl_fit(const Image *src, int bw, int bh) {
  if (!src || src->w <= 0 || src->h <= 0) return NULL;
  double sx = (double)bw / src->w, sy = (double)bh / src->h, sc = sx < sy ? sx : sy;
  int w = (int)(src->w * sc + 0.5), h = (int)(src->h * sc + 0.5);
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  return img_scale(src, w, h);
}

static int pl_file_ok(const char *p) {
  FILE *f = fopen(p, "rb");
  if (!f) return 0;
  int c = fgetc(f);
  fclose(f);
  return c != EOF;
}

static void *pl_worker(void *arg) {
  PlJobs *J = (PlJobs *)arg;
  UI *ui = J->ui;
  for (int k = 0; k < J->n; k++) {
    pthread_mutex_lock(&pl_mu); int cur = plr_gen; pthread_mutex_unlock(&pl_mu);
    if (cur != J->gen) break;
    char path[340], tmp[360];
    cover_cache_path(ui, J->url[k], path, sizeof(path));
    if (!pl_file_ok(path)) {
      if (!ui->cover_fetch) continue;
      snprintf(tmp, sizeof(tmp), "%s.tmp", path);
      if (ui->cover_fetch(J->url[k], tmp, ui->cover_ud) != 0 || rename(tmp, path) != 0) { remove(tmp); continue; }
    }
    Image *raw = img_load(path);
    if (!raw) { remove(path); continue; }                       /* arquivo ruim: baixa de novo na proxima vez */
    Image *a, *b;
    if (J->logo[k]) { a = pl_fit(raw, PL_LW - 6, PL_LW - 6); b = pl_fit(raw, PL_ELW - 14, PL_ELW - 14); }
    else            { a = pl_fit(raw, PL_IW - 10, PL_IH - 6);  b = pl_fit(raw, PL_EIW - 16, PL_EIH - 8); }
    img_free(raw);
    pthread_mutex_lock(&pl_mu);
    if (plr_gen == J->gen) {
      if (plr_im[k][0]) img_free(plr_im[k][0]);
      if (plr_im[k][1]) img_free(plr_im[k][1]);
      plr_im[k][0] = a; plr_im[k][1] = b; plr_rdy[k] = 1; plr_pending = 1;
      a = b = NULL;
    }
    pthread_mutex_unlock(&pl_mu);
    if (a) img_free(a);
    if (b) img_free(b);
  }
  free(J);
  return NULL;
}

static void pl_free_images(UI *ui) {
  for (int k = 0; k < PL_MAXU; k++)
    for (int j = 0; j < 2; j++) if (ui->pl_im[k][j]) { img_free(ui->pl_im[k][j]); ui->pl_im[k][j] = NULL; }
}

/* indice da url na lista do quadro (imagens e logos repetidos sao baixados uma vez so) */
static int pl_url_add(UI *ui, PlJobs *J, const char *u, int logo) {
  if (!u || strncmp(u, "https://", 8)) return -1;
  for (int k = 0; k < J->n; k++) if (!strcmp(J->url[k], u)) return k;
  if (J->n >= PL_MAXU) return -1;
  snprintf(J->url[J->n], sizeof(J->url[0]), "%s", u);
  J->logo[J->n] = logo;
  return J->n++;
}

static void pl_load_images(UI *ui) {
  const CatPlBoard *b = pl_board_cur(ui);
  pthread_mutex_lock(&pl_mu);
  plr_gen++;
  for (int k = 0; k < PL_MAXU; k++) {
    for (int j = 0; j < 2; j++) if (plr_im[k][j]) { img_free(plr_im[k][j]); plr_im[k][j] = NULL; }
    plr_rdy[k] = 0;
  }
  plr_pending = 0;
  int gen = plr_gen;
  pthread_mutex_unlock(&pl_mu);
  pl_free_images(ui);
  for (int i = 0; i < CAT_MAX_PLO; i++) ui->pl_oi[i] = ui->pl_ol[i] = -1;
  if (!b) return;
  PlJobs *J = (PlJobs *)calloc(1, sizeof(PlJobs));
  if (!J) return;
  J->ui = ui; J->gen = gen;
  for (int i = 0; i < b->nopt; i++) {                            /* primeiro as imagens dos placares, depois os logos */
    ui->pl_oi[i] = pl_url_add(ui, J, b->opt[i].img, 0);
  }
  for (int i = 0; i < b->nopt; i++) ui->pl_ol[i] = pl_url_add(ui, J, b->opt[i].logo, 1);
  if (J->n == 0 || !ui->cover_dir[0]) { free(J); return; }
  pthread_t t; pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&t, &at, pl_worker, J) != 0) free(J);
  pthread_attr_destroy(&at);
}

static void pl_poll(UI *ui) {
  if (!plr_pending) return;
  pthread_mutex_lock(&pl_mu);
  for (int k = 0; k < PL_MAXU; k++) if (plr_rdy[k]) {
    for (int j = 0; j < 2; j++) { if (ui->pl_im[k][j]) img_free(ui->pl_im[k][j]); ui->pl_im[k][j] = plr_im[k][j]; plr_im[k][j] = NULL; }
    plr_rdy[k] = 0;
  }
  plr_pending = 0;
  pthread_mutex_unlock(&pl_mu);
  ui->dirty = 1;
}

/* ---- o que esta instalado: compara o tamanho dos JK_PLACAR_*.cpk do console com o de cada opcao */
static long long pl_fsize(const char *p) {
  FILE *f = fopen(p, "rb");
  if (!f) return -1;
  long long n = -1;
  if (fseek(f, 0, SEEK_END) == 0) n = (long long)ftell(f);
  fclose(f);
  return n;
}
static void pl_scan(UI *ui) {
  const CatPlBoard *b = pl_board_cur(ui);
  ui->pl_scanned = 1;
  for (int c = 0; c < 4; c++) ui->pl_inst[c] = -1;
  if (!b || !b->dest[0]) return;
  for (int c = 0; c < 4; c++) {
    char p[200];
    snprintf(p, sizeof(p), "%s/JK_PLACAR_%s.cpk", b->dest, PL_FILE[c]);
    long long n = pl_fsize(p);
    if (n <= 0) continue;
    for (int i = 0; i < b->nopt; i++)
      if (b->opt[i].comp == c && b->opt[i].size == (unsigned long long)n) { ui->pl_inst[c] = i; break; }
  }
}
static int pl_pack_installed(UI *ui, const CatPlPack *p) {
  for (int c = 0; c < 4; c++) if (ui->pl_inst[c] != p->opt[c]) return 0;
  return 1;
}

static void pl_open(UI *ui) {
  ui->pl_sel = 0; ui->pl_ed = 0; ui->pl_row = 0;
  pl_scan(ui);
  pl_load_images(ui);
  const CatPlBoard *b = pl_board_cur(ui);
  if (b) for (int p = 0; p < b->npack; p++) if (pl_pack_installed(ui, &b->pack[p])) { ui->pl_sel = p; break; }   /* abre no combo que ja esta no console */
  ui->dirty = 1;
}

/* fundo: gradiente + linhas do campo bem apagadas + varredura (gerado uma vez) */
static void pl_build_bg(UI *ui) {
  Image *bg = img_new(W, H);
  if (!bg) return;
  gfx_vgradient(bg, 0, 0, W, H, RGB(0x0b, 0x15, 0x2a), RGB(0x05, 0x08, 0x12));
  uint32_t ln = RGB(0x42, 0xe6, 0xa4);
  int cx = W / 2, cy = 565;
  gfx_ring_arc(bg, cx, cy, 250, 3, ln, 16);                   /* circulo central */
  gfx_circle(bg, cx, cy, 8, ln, 22);
  gfx_fill(bg, cx - 1, 130, 3, 860, ln, 14);                  /* linha do meio */
  gfx_fill(bg, 40, 130, 3, 860, ln, 12);                      /* laterais */
  gfx_fill(bg, W - 43, 130, 3, 860, ln, 12);
  gfx_fill(bg, 40, 330, 220, 3, ln, 12); gfx_fill(bg, 40, 797, 220, 3, ln, 12); gfx_fill(bg, 257, 330, 3, 470, ln, 12);   /* grande area esq. */
  gfx_fill(bg, W - 260, 330, 220, 3, ln, 12); gfx_fill(bg, W - 260, 797, 220, 3, ln, 12); gfx_fill(bg, W - 260, 330, 3, 470, ln, 12);
  for (int y = 0; y < H; y += 4) gfx_fill(bg, 0, y, W, 1, C_BLACK, 26);   /* linhas de varredura (tela de TV) */
  gfx_vfade(bg, 0, 0, W, 260, RGB(0x42, 0xe6, 0xa4), 22, 0);   /* brilho verde no alto */
  ui->pl_bg = bg;
}

/* ---- instalacao: 4 CPKs + arquivos comuns do quadro (plugin, regras, base do JK-MOD) */
static void pl_install(UI *ui, const int *opt, const char *what) {
  const CatPlBoard *b = pl_board_cur(ui);
  if (!b) return;
  if (ui->locked) {
    ui_toast(ui, ui->offline ? "Sem conexão com o servidor. Downloads indisponíveis agora." : "Ative seu plano para baixar.", UI_TOAST_ERR);
    return;
  }
  if (ui->busy) { ui_toast(ui, "Já existe um pedido em andamento. Aguarde.", UI_TOAST_INFO); return; }
  for (int c = 0; c < 4; c++) if (opt[c] < 0 || opt[c] >= b->nopt || !b->opt[opt[c]].url[0]) { ui_toast(ui, "Este combo está incompleto no catálogo.", UI_TOAST_ERR); return; }
  static CatButton cb;                                          /* grande: fora da pilha (o app copia ao iniciar) */
  memset(&cb, 0, sizeof(cb));
  snprintf(cb.label, sizeof(cb.label), "PLACARES");
  snprintf(cb.name, sizeof(cb.name), "JK Placares: %s", what);
  snprintf(cb.url, sizeof(cb.url), "%s", b->opt[opt[0]].url);
  snprintf(cb.action, sizeof(cb.action), "download");
  snprintf(cb.dest, sizeof(cb.dest), "%s", b->dest);
  cb.size = b->opt[opt[0]].size;
  size_t L = 0;
  for (int c = 1; c < 4; c++) L += (size_t)snprintf(cb.extra + L, sizeof(cb.extra) - L, "%s%s", L ? "|" : "", b->opt[opt[c]].url);
  if (b->extra[0] && L + 1 + strlen(b->extra) < sizeof(cb.extra)) snprintf(cb.extra + L, sizeof(cb.extra) - L, "|%s", b->extra);
  ui_toast(ui, "Baixando os placares para o PS4...", UI_TOAST_INFO);
  ui->busy = 1;
  ui->updating = 0;
  ui->pl_scanned = 0;                                           /* rele o que ficou instalado quando terminar */
  if (ui->fn) ui->fn(&cb, ui->user);
}

static void pl_ed_open(UI *ui) {
  const CatPlBoard *b = pl_board_cur(ui);
  if (!b) return;
  for (int c = 0; c < 4; c++) {
    int p = ui->pl_inst[c];
    if (p < 0 && b->npack > 0) p = b->pack[0].opt[c];
    if (p < 0) for (int i = 0; i < b->nopt; i++) if (b->opt[i].comp == c) { p = i; break; }
    ui->pl_pick[c] = p;
  }
  ui->pl_ed = 1; ui->pl_row = 0; ui->dirty = 1;
}

/* proxima/anterior opcao da competicao c (dir = +1 / -1), com volta */
static void pl_cycle(UI *ui, int c, int dir) {
  const CatPlBoard *b = pl_board_cur(ui);
  if (!b) return;
  int idx[CAT_MAX_PLO], n = 0, at = 0;
  for (int i = 0; i < b->nopt; i++) if (b->opt[i].comp == c) { if (i == ui->pl_pick[c]) at = n; idx[n++] = i; }
  if (n <= 0) return;
  at = (at + dir + n) % n;
  ui->pl_pick[c] = idx[at];
  ui->dirty = 1;
}

static void pl_key(UI *ui, int key) {
  const CatPlBoard *b = pl_board_cur(ui);
  int nc = pl_ncards(b);
  if (ui->pl_ed) {
    switch (key) {
      case UI_UP:    if (ui->pl_row > 0) { ui->pl_row--; ui->dirty = 1; } break;
      case UI_DOWN:  if (ui->pl_row < 3) { ui->pl_row++; ui->dirty = 1; } break;
      case UI_LEFT:  pl_cycle(ui, ui->pl_row, -1); break;
      case UI_RIGHT: pl_cycle(ui, ui->pl_row, +1); break;
      case UI_OK:    pl_install(ui, ui->pl_pick, "Monte o seu"); break;
      case UI_BACK:  ui->pl_ed = 0; ui->dirty = 1; break;
      case UI_SQUARE: trigger_update(ui); break;
      default: break;
    }
    return;
  }
  int sel = ui->pl_sel, page = sel / PL_PER, slot = sel % PL_PER, col = slot % 3, row = slot / 3;
  switch (key) {
    case UI_LEFT:
      if (col > 0) sel--;
      else if (page > 0) { sel = (page - 1) * PL_PER + row * 3 + 2; if (sel >= nc) sel = nc - 1; }
      break;
    case UI_RIGHT:
      if (col < 2 && sel + 1 < nc) sel++;
      else if ((page + 1) * PL_PER < nc) { sel = (page + 1) * PL_PER + row * 3; if (sel >= nc) sel = nc - 1; }
      break;
    case UI_UP:    if (row == 1) sel -= 3; break;
    case UI_DOWN:
      if (row == 0) { if (sel + 3 < nc) sel += 3; else if (page * PL_PER + 3 < nc) sel = nc - 1; }
      break;
    case UI_L1: case UI_R1:
      if (ui->cat && ui->cat->nplb > 1) {
        int n = ui->cat->nplb;
        ui->pl_b = (ui->pl_b + (key == UI_R1 ? 1 : n - 1)) % n;
        pl_open(ui);
      }
      return;
    case UI_OK:
      if (!b || nc <= 0) break;
      if (b->monte && sel == nc - 1) pl_ed_open(ui);
      else if (sel < b->npack) pl_install(ui, b->pack[sel].opt, b->pack[sel].name);
      break;
    case UI_BACK: ui->detail = 0; ui->pl_ed = 0; ui->dirty = 1; rebuild_fbg(ui); return;
    case UI_SQUARE: trigger_update(ui); return;
    default: break;
  }
  if (nc > 0 && sel >= 0 && sel < nc && sel != ui->pl_sel) { ui->pl_sel = sel; ui->dirty = 1; }
}

static void pl_tick(UI *ui) {
  if (!pl_active(ui)) return;
  pl_poll(ui);
  if (!ui->pl_scanned && !ui->busy) { pl_scan(ui); ui->dirty = 1; }
}

/* ---- desenho */
static void pl_blit_center(UI *ui, const Image *im, int x, int y, int w, int h) {
  if (!im) return;
  gfx_blit(ui->s, im, x + (w - im->w) / 2, y + (h - im->h) / 2, 0, 255);
}

static void pl_mb(unsigned long long n, char *out, int cap) {
  unsigned long long mb = (n + 512 * 1024) / (1024 * 1024);
  snprintf(out, (size_t)cap, "%llu MB", mb ? mb : 1);
}

static void pl_glow(Image *s, int x, int y, int w, int h, int r, uint32_t col) {   /* borda em foco com brilho */
  gfx_rrect(s, x - 7, y - 7, w + 14, h + 14, r + 7, col, 40, 4);
  gfx_rrect(s, x - 3, y - 3, w + 6, h + 6, r + 3, col, 90, 3);
  gfx_rrect(s, x, y, w, h, r, col, 255, 4);
}

/* imagem do placar + logo de uma opcao (placeholder com o nome enquanto a imagem nao chega) */
static void pl_draw_opt(UI *ui, const CatPlOpt *o, int oi, int x, int y, int iw, int ih, int lw, int big) {
  Image *s = ui->s;
  gfx_rrect(s, x, y, iw, ih, 10, C_DEEP, 255, 0);
  int lx = x + iw + (big ? 74 : 8), ly = y + (ih - lw) / 2;
  gfx_rrect(s, lx, ly, lw, lw, 12, C_DEEP, 255, 0);
  gfx_rrect(s, lx, ly, lw, lw, 12, C_EDGE, 255, 1);
  if (!o) return;
  int ki = ui->pl_oi[oi], kl = ui->pl_ol[oi];
  Image *pi = ki >= 0 ? ui->pl_im[ki][big] : NULL, *pg = kl >= 0 ? ui->pl_im[kl][big] : NULL;
  if (pi) pl_blit_center(ui, pi, x, y, iw, ih);
  else text_draw_in(s, ui->fb, big ? 30 : 18, x, iw, y + ih / 2 + (big ? 11 : 7), o->name, C_MUTED, 200, 1, 1);
  if (pg) pl_blit_center(ui, pg, lx, ly, lw, lw);
}

static void pl_button(UI *ui, int x, int y, int w, int h, const char *txt, int focused, int done) {
  Image *s = ui->s;
  uint32_t bgc = done ? C_BASE : (focused ? C_FOCUS : C_PANEL2), fg = done ? C_TEXT : (focused ? C_BG : C_TEXT);
  gfx_rrect(s, x, y, w, h, 12, bgc, 255, 0);
  if (done) gfx_rrect(s, x, y, w, h, 12, C_BASE_EDGE, 255, 2);
  else if (!focused) gfx_rrect(s, x, y, w, h, 12, C_EDGE, 255, 1);
  text_draw_in(s, ui->fb, 22, x, w, y + h / 2 + 8, txt, fg, 255, 2, 1);
}

static void pl_draw_pack(UI *ui, const CatPlBoard *b, int p, int x, int y, int focused) {
  Image *s = ui->s;
  const CatPlPack *pk = &b->pack[p];
  int inst = pl_pack_installed(ui, pk);
  gfx_rrect(s, x, y, PL_CW, PL_CH, 18, focused ? C_PANEL2 : C_PANEL, 240, 0);
  if (focused) pl_glow(s, x, y, PL_CW, PL_CH, 18, C_FOCUS);
  else gfx_rrect(s, x, y, PL_CW, PL_CH, 18, inst ? C_BASE_EDGE : C_EDGE, 255, inst ? 2 : 1);
  gfx_fill(s, x + 24, y + 1, 110, 3, inst ? C_ACCENT : (focused ? C_FOCUS : C_ACCENT), focused || inst ? 255 : 110);   /* risco "neon" do topo */
  unsigned long long tot = 0;
  for (int c = 0; c < 4; c++) {
    int ry = y + PL_PAD + c * PL_RS;
    int oi = pk->opt[c];
    const CatPlOpt *o = (oi >= 0 && oi < b->nopt) ? &b->opt[oi] : NULL;
    if (o) tot += o->size;
    T(ui, 1, 14, x + PL_PAD + 2, ry + 13, PL_COMP[c], C_MUTED, 255, 1);
    pl_draw_opt(ui, o, oi, x + PL_PAD, ry + 18, PL_IW, PL_IH, PL_LW, 0);
  }
  char t[64], mb[24];
  pl_mb(tot, mb, sizeof(mb));
  if (inst) snprintf(t, sizeof(t), "\xE2\x9C\x93  INSTALADO");
  else snprintf(t, sizeof(t), "BAIXAR  \xC2\xB7  %s", mb);
  pl_button(ui, x + PL_PAD, y + PL_CH - PL_PAD - 48, PL_CW - 2 * PL_PAD, 48, t, focused && !inst, inst);
  if (inst && focused) gfx_rrect(s, x + PL_PAD, y + PL_CH - PL_PAD - 48, PL_CW - 2 * PL_PAD, 48, 12, C_FOCUS, 255, 3);
}

static int pl_count(const CatPlBoard *b, int c) { int n = 0; for (int i = 0; i < b->nopt; i++) if (b->opt[i].comp == c) n++; return n; }

static void pl_draw_monte(UI *ui, const CatPlBoard *b, int x, int y, int focused) {
  Image *s = ui->s;
  gfx_rrect(s, x, y, PL_CW, PL_CH, 18, focused ? C_PANEL2 : C_PANEL, 240, 0);
  if (focused) pl_glow(s, x, y, PL_CW, PL_CH, 18, C_FOCUS);
  else gfx_rrect(s, x, y, PL_CW, PL_CH, 18, C_FOCUS, 150, 2);
  /* icone: 4 barrinhas de placar empilhadas + "+" */
  int ix = x + PL_CW - 96, iy = y + 26;
  for (int k = 0; k < 4; k++) gfx_rrect(s, ix, iy + k * 14, 58 - k * 8, 9, 4, k == 0 ? C_FOCUS : C_ACCENT, 230 - k * 40, 0);
  gfx_circle(s, ix + 62, iy + 46, 15, C_FOCUS, 255);
  gfx_fill(s, ix + 55, iy + 44, 15, 4, C_BG, 255);
  gfx_fill(s, ix + 60, iy + 39, 4, 15, C_BG, 255);
  T(ui, 1, 34, x + 22, y + 58, "MONTE O SEU", C_FOCUS, 255, 2);
  T(ui, 0, 18, x + 22, y + 90, "Escolha 1 placar para cada competição", C_MUTED, 255, 0);
  for (int c = 0; c < 4; c++) {
    int ry = y + 118 + c * 62;
    gfx_rrect(s, x + PL_PAD, ry, PL_CW - 2 * PL_PAD, 50, 10, C_DEEP, 255, 0);
    gfx_fill(s, x + PL_PAD, ry + 10, 4, 30, C_ACCENT, 255);
    T(ui, 1, 18, x + PL_PAD + 18, ry + 32, PL_COMP[c], C_TEXT, 255, 1);
    char n[24];
    snprintf(n, sizeof(n), "%d opções", pl_count(b, c));
    int w = TW(ui, 1, 17, n, 0);
    T(ui, 1, 17, x + PL_CW - PL_PAD - 16 - w, ry + 32, n, C_ACCENT, 255, 0);
  }
  pl_button(ui, x + PL_PAD, y + PL_CH - PL_PAD - 48, PL_CW - 2 * PL_PAD, 48, "MONTAR", focused, 0);
}

static void pl_draw_header(UI *ui, const CatPlBoard *b) {
  Image *s = ui->s;
  gfx_fill(s, 0, 0, W, 112, C_BG, 150);
  gfx_fill(s, 0, 112, W, 2, C_ACCENT, 140);
  gfx_fill(s, 0, 114, W, 6, C_ACCENT, 18);
  int x = PL_X0;
  if (ui->logo) { gfx_blit(s, ui->logo, x, 12, 0, 255); x += 88 + 26; }
  gfx_fill(s, x, 22, 3, 68, C_ACCENT, 200);
  x += 20;
  T(ui, 1, 18, x, 44, "JK-MOD", C_ACCENT, 255, 6);
  T(ui, 1, 46, x - 2, 92, "PLACARES", C_TEXT, 255, 4);
  if (!ui->cat) return;
  /* quadros (patches): L1 / R1 */
  int n = ui->cat->nplb, cw[CAT_MAX_PLB], tot = 0;
  for (int i = 0; i < n; i++) { cw[i] = TW(ui, 1, 20, ui->cat->plb[i].title, 2) + 48; tot += cw[i] + 8; }
  int kx = W - PL_X0, y = 22;
  if (n > 1) { int kw = TW(ui, 1, 17, "R1", 0) + 18; kx -= kw; gfx_rrect(s, kx, y + 8, kw, 30, 8, C_EDGE, 255, 2); text_draw_in(s, ui->fb, 17, kx, kw, y + 30, "R1", C_MUTED, 255, 0, 1); kx -= 12; }
  int px = kx - tot - 4;
  gfx_rrect(s, px, y, tot + 4, 46, 14, C_PANEL, 255, 0);
  int cx = px + 6;
  for (int i = 0; i < n; i++) {
    int on = i == ui->pl_b;
    if (on) gfx_rrect(s, cx, y + 5, cw[i], 36, 10, C_ACCENT, 255, 0);
    text_draw_in(s, ui->fb, 20, cx, cw[i], y + 31, ui->cat->plb[i].title, on ? C_DARKTXT : C_MUTED, 255, 2, 1);
    cx += cw[i] + 8;
  }
  if (n > 1) { int kw = TW(ui, 1, 17, "L1", 0) + 18; int lx = px - 12 - kw; gfx_rrect(s, lx, y + 8, kw, 30, 8, C_EDGE, 255, 2); text_draw_in(s, ui->fb, 17, lx, kw, y + 30, "L1", C_MUTED, 255, 0, 1); }
  if (b && b->cusa[0]) {
    char sub[64];
    snprintf(sub, sizeof(sub), "PS4  \xC2\xB7  %s", b->cusa);
    int w = TW(ui, 0, 16, sub, 2);
    T(ui, 0, 16, W - PL_X0 - w, 96, sub, C_MUTED, 200, 2);
  }
}

static void pl_draw_msg(UI *ui) {                                /* aviso/andamento: no alto, sem cobrir os cards */
  if (!(ui->toast_left > 0 || ui->busy)) return;
  Image *s = ui->s;
  uint32_t edge = ui->toast_kind == UI_TOAST_ERR ? C_ERR : (ui->toast_kind == UI_TOAST_OK ? C_ACCENT : C_FOCUS);
  char buf[256];
  text_fit(ui->f, 22, ui->toast, 760, buf, sizeof(buf));
  int tw = TW(ui, 0, 22, buf, 0), pad = 26, ex = ui->busy ? 40 : 0;
  int w = tw + pad * 2 + ex, h = 52, x = 760 - w / 2 + 200, y = 30;
  if (x < 640) x = 640;
  gfx_rrect(s, x, y, w, h, h / 2, C_PANEL2, 250, 0);
  gfx_rrect(s, x, y, w, h, h / 2, edge, 255, 3);
  if (ui->busy) draw_spinner(ui, x + pad + 6, y + h / 2);
  T(ui, 0, 22, x + pad + ex, y + h / 2 + 8, buf, C_TEXT, 255, 0);
}

static void pl_draw_editor(UI *ui, const CatPlBoard *b) {
  Image *s = ui->s;
  gfx_fill(s, 0, 0, W, 990, C_BLACK, 170);
  int px = 150, py = 140, pw = 1620, ph = 830;
  gfx_rrect(s, px, py, pw, ph, 24, C_PANEL, 252, 0);
  gfx_rrect(s, px, py, pw, ph, 24, C_FOCUS, 160, 2);
  gfx_fill(s, px + 40, py + 1, 160, 3, C_FOCUS, 255);
  T(ui, 1, 40, px + 40, py + 62, "MONTE O SEU", C_FOCUS, 255, 3);
  T(ui, 0, 20, px + 40, py + 96, "\xE2\x96\xB2\xE2\x96\xBC escolhe a competição   \xE2\x97\x80\xE2\x96\xB6 troca o placar   X baixa o seu combo", C_MUTED, 255, 0);
  { int w = TW(ui, 1, 20, b->title, 2) + 40; gfx_rrect(s, px + pw - 40 - w, py + 34, w, 40, 12, C_ACCENT, 255, 0);
    text_draw_in(s, ui->fb, 20, px + pw - 40 - w, w, py + 61, b->title, C_DARKTXT, 255, 2, 1); }
  unsigned long long tot = 0;
  for (int c = 0; c < 4; c++) {
    int ry = py + 124 + c * 148, rx = px + 40, rw = pw - 80, rh = 134;
    int foc = ui->pl_row == c;
    gfx_rrect(s, rx, ry, rw, rh, 16, foc ? C_PANEL2 : C_DEEP, 255, 0);
    if (foc) pl_glow(s, rx, ry, rw, rh, 16, C_FOCUS); else gfx_rrect(s, rx, ry, rw, rh, 16, C_EDGE, 255, 1);
    int oi = ui->pl_pick[c];
    const CatPlOpt *o = (oi >= 0 && oi < b->nopt) ? &b->opt[oi] : NULL;
    if (o) tot += o->size;
    T(ui, 1, 21, rx + 28, ry + 46, PL_COMP[c], C_TEXT, 255, 1);
    if (o) {
      char nm[64]; text_fit(ui->fb, 24, o->name, 300, nm, sizeof(nm));
      T(ui, 1, 24, rx + 28, ry + 84, nm, C_ACCENT, 255, 0);
      int at = 0, n = 0;
      for (int i = 0; i < b->nopt; i++) if (b->opt[i].comp == c) { if (i == oi) at = n; n++; }
      char pos[24]; snprintf(pos, sizeof(pos), "%d de %d", at + 1, n);
      T(ui, 0, 18, rx + 28, ry + 116, pos, C_MUTED, 255, 0);
    }
    uint32_t ac = foc ? C_FOCUS : C_MUTED;
    text_draw_in(s, ui->fb, 34, rx + 330, 40, ry + rh / 2 + 12, "\xE2\x97\x80", ac, foc ? 255 : 120, 0, 1);
    pl_draw_opt(ui, o, oi, rx + 380, ry + 12, PL_EIW, PL_EIH, PL_ELW, 1);
    text_draw_in(s, ui->fb, 34, rx + 380 + PL_EIW + 12, 40, ry + rh / 2 + 12, "\xE2\x96\xB6", ac, foc ? 255 : 120, 0, 1);
  }
  char t[64], mb[24];
  pl_mb(tot, mb, sizeof(mb));
  snprintf(t, sizeof(t), "BAIXAR MEU COMBO  \xC2\xB7  %s", mb);
  int bw = 560;
  pl_button(ui, px + (pw - bw) / 2, py + ph - 78, bw, 56, t, 1, 0);
}

static void pl_draw(UI *ui) {
  Image *s = ui->s;
  if (!ui->pl_bg) pl_build_bg(ui);
  if (ui->pl_bg) gfx_blit_surface(s, ui->pl_bg); else gfx_fill(s, 0, 0, W, H, C_BG, 255);
  const CatPlBoard *b = pl_board_cur(ui);
  pl_draw_header(ui, b);
  int nc = pl_ncards(b);
  if (!b || nc == 0) {
    text_draw_in(s, ui->fb, 30, 0, W, 540, "Nenhum placar disponível agora.", C_MUTED, 255, 0, 1);
    pl_draw_msg(ui);
    return;
  }
  int page = ui->pl_sel / PL_PER, pages = (nc + PL_PER - 1) / PL_PER;
  for (int k = 0; k < PL_PER; k++) {
    int i = page * PL_PER + k;
    if (i >= nc) break;
    int x = PL_X0 + (k % 3) * (PL_CW + PL_GX), y = PL_Y0 + (k / 3) * (PL_CH + PL_GY);
    if (b->monte && i == nc - 1) pl_draw_monte(ui, b, x, y, i == ui->pl_sel && !ui->pl_ed);
    else pl_draw_pack(ui, b, i, x, y, i == ui->pl_sel && !ui->pl_ed);
  }
  if (pages > 1) {                                               /* bolinhas das paginas */
    int dx = W / 2 - (pages * 26) / 2;
    for (int p = 0; p < pages; p++) gfx_circle(s, dx + p * 26 + 13, 975, p == page ? 7 : 5, p == page ? C_FOCUS : C_MUTED, p == page ? 255 : 140);
  }
  if (ui->pl_ed) pl_draw_editor(ui, b);
  pl_draw_msg(ui);
}

static void pl_footer(UI *ui, int *px) {
  int x = *px;
  if (ui->pl_ed) {
    x = hint(ui, x, "\xE2\x96\xB2 \xE2\x96\xBC", "Competição");
    x = hint(ui, x, "\xE2\x97\x80 \xE2\x96\xB6", "Trocar placar");
    x = hint(ui, x, "X", ui->locked ? "Bloqueado" : "Baixar meu combo");
    x = hint(ui, x, "O", "Voltar");
  } else {
    const CatPlBoard *b = pl_board_cur(ui);
    x = hint(ui, x, "\xE2\x97\x80 \xE2\x96\xB2 \xE2\x96\xBC \xE2\x96\xB6", "Escolher combo");
    x = hint(ui, x, "X", ui->locked ? "Bloqueado" : ((b && b->monte && ui->pl_sel == pl_ncards(b) - 1) ? "Montar" : "Baixar"));
    if (ui->cat && ui->cat->nplb > 1) x = hint(ui, x, "L1  R1", "Trocar patch");
    x = hint(ui, x, "O", "Voltar");
  }
  if (update_available(ui)) x = hint(ui, x, "\xE2\x96\xA1", "Atualizar app");
  *px = x;
}
