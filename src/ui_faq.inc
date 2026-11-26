/* ------------------------------------------------------------------ v1.41: card Duvidas (tag AJUDA) em telas
 * Linhas FAQ do catalogo: uma imagem 1920x1080 por assunto. As telas passam sozinhas a cada FQ_SECS segundos;
 * L1/R1 ou setas trocam; X abre o video da tela (se tiver) ou pausa/continua; O volta. (triangulo e sair da conta no app)
 * Sem linhas FAQ no catalogo o card continua como antes. Incluido no ui.c. */

#define FQ_W     1440
#define FQ_H     810
#define FQ_X     ((W - FQ_W) / 2)
#define FQ_Y     128
#define FQ_SECS  10
#define FQ_TICKS (FQ_SECS * 60)

static int is_ajuda(const CatGame *g) { return g && !strcmp(g->tag, "AJUDA"); }
static int fq_active(UI *ui) { return ui->mode == MODE_CATALOG && ui->detail && is_ajuda(cur_game(ui)) && ui->cat && ui->cat->nfaq > 0; }

static pthread_mutex_t fq_mu = PTHREAD_MUTEX_INITIALIZER;
static int fqr_gen = 0, fqr_pending = 0;
static Image *fqr_im[CAT_MAX_FAQ];

typedef struct { UI *ui; int gen, n, first; char url[CAT_MAX_FAQ][256]; } FqJobs;

static void *fq_worker(void *arg) {
  FqJobs *J = (FqJobs *)arg;
  UI *ui = J->ui;
  for (int s = 0; s < J->n; s++) {
    int k = (J->first + s) % J->n;                               /* comeca pela tela que esta na frente */
    pthread_mutex_lock(&fq_mu); int cur = fqr_gen; pthread_mutex_unlock(&fq_mu);
    if (cur != J->gen) break;
    char path[340], tmp[360];
    cover_cache_path(ui, J->url[k], path, sizeof(path));
    FILE *f = fopen(path, "rb");
    int have = 0;
    if (f) { have = fgetc(f) != EOF; fclose(f); }
    if (!have) {
      if (!ui->cover_fetch) continue;
      snprintf(tmp, sizeof(tmp), "%s.tmp", path);
      if (ui->cover_fetch(J->url[k], tmp, ui->cover_ud) != 0 || rename(tmp, path) != 0) { remove(tmp); continue; }
    }
    Image *raw = img_load(path);
    if (!raw) { remove(path); continue; }
    Image *a = (raw->w == FQ_W && raw->h == FQ_H) ? raw : img_scale(raw, FQ_W, FQ_H);
    if (a != raw) img_free(raw);
    pthread_mutex_lock(&fq_mu);
    if (fqr_gen == J->gen) { if (fqr_im[k]) img_free(fqr_im[k]); fqr_im[k] = a; a = NULL; fqr_pending = 1; }
    pthread_mutex_unlock(&fq_mu);
    if (a) img_free(a);
  }
  free(J);
  return NULL;
}

static void fq_poll(UI *ui) {
  if (!fqr_pending) return;
  pthread_mutex_lock(&fq_mu);
  for (int k = 0; k < CAT_MAX_FAQ; k++) if (fqr_im[k]) { if (ui->fq_im[k]) img_free(ui->fq_im[k]); ui->fq_im[k] = fqr_im[k]; fqr_im[k] = NULL; }
  fqr_pending = 0;
  pthread_mutex_unlock(&fq_mu);
  ui->dirty = 1;
}

static void fq_open(UI *ui) {
  ui->fq_i = 0; ui->fq_t = 0; ui->fq_pause = 0;
  ui->dirty = 1;
  if (!ui->cat || !ui->cover_dir[0]) return;
  int need = 0;
  for (int k = 0; k < ui->cat->nfaq; k++) if (!ui->fq_im[k]) need = 1;
  if (!need) return;                                             /* ja carregadas nesta abertura do app */
  pthread_mutex_lock(&fq_mu);
  int gen = ++fqr_gen;
  pthread_mutex_unlock(&fq_mu);
  FqJobs *J = (FqJobs *)calloc(1, sizeof(FqJobs));
  if (!J) return;
  J->ui = ui; J->gen = gen; J->n = ui->cat->nfaq; J->first = 0;
  for (int k = 0; k < J->n; k++) snprintf(J->url[k], sizeof(J->url[0]), "%s", ui->cat->faq[k].img);
  pthread_t t; pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&t, &at, fq_worker, J) != 0) free(J);
  pthread_attr_destroy(&at);
}

static void fq_go(UI *ui, int dir) {
  int n = ui->cat ? ui->cat->nfaq : 0;
  if (n <= 0) return;
  ui->fq_i = (ui->fq_i + dir + n) % n;
  ui->fq_t = 0;
  ui->dirty = 1;
}

static void fq_key(UI *ui, int key) {
  switch (key) {
    case UI_L1: case UI_LEFT:  fq_go(ui, -1); break;
    case UI_R1: case UI_RIGHT: fq_go(ui, +1); break;
    case UI_OK: {
      const CatFaq *q = &ui->cat->faq[ui->fq_i];
      if (q->video[0]) { ui->fq_pause = 1; vid_open_url(ui, q->title, q->video); }
      else { ui->fq_pause = !ui->fq_pause; ui->fq_t = 0; ui->dirty = 1; }
      break;
    }
    case UI_BACK: ui->detail = 0; ui->dirty = 1; rebuild_fbg(ui); break;
    case UI_SQUARE: trigger_update(ui); break;
    default: break;
  }
}

static void fq_tick(UI *ui) {
  if (!fq_active(ui)) return;
  fq_poll(ui);
  if (ui->fq_pause || ui->vid_mode || ui->shop || !ui->fq_im[ui->fq_i]) return;
  if (++ui->fq_t >= FQ_TICKS) fq_go(ui, +1);
  else if (ui->fq_t % 12 == 0) ui->dirty = 1;                    /* barra de tempo */
}

static void fq_draw(UI *ui) {
  Image *s = ui->s;
  if (!ui->pl_bg) pl_build_bg(ui);
  if (ui->pl_bg) gfx_blit_surface(s, ui->pl_bg); else gfx_fill(s, 0, 0, W, H, C_BG, 255);
  const CatFaq *q = &ui->cat->faq[ui->fq_i];
  int n = ui->cat->nfaq;
  /* topo */
  gfx_fill(s, 0, 0, W, 112, C_BG, 150);
  gfx_fill(s, 0, 112, W, 2, C_ACCENT, 140);
  int x = PL_X0;
  if (ui->logo) { gfx_blit(s, ui->logo, x, 12, 0, 255); x += 88 + 26; }
  gfx_fill(s, x, 22, 3, 68, C_ACCENT, 200);
  x += 20;
  T(ui, 1, 18, x, 44, "JK PATCHS", C_ACCENT, 255, 6);
  T(ui, 1, 46, x - 2, 92, "DÚVIDAS", C_TEXT, 255, 4);
  char top[160], tt[110];
  text_fit(ui->f, 22, q->title, 760, tt, sizeof(tt));
  snprintf(top, sizeof(top), "%d de %d  \xC2\xB7  %s%s", ui->fq_i + 1, n, tt, ui->fq_pause ? "  \xC2\xB7  PAUSADO" : "");
  int w = TW(ui, 0, 22, top, 0);
  T(ui, 0, 22, W - PL_X0 - w, 68, top, ui->fq_pause ? C_FOCUS : C_MUTED, 255, 0);
  /* tela */
  Image *im = ui->fq_im[ui->fq_i];
  if (im) gfx_blit(s, im, FQ_X, FQ_Y, 14, 255);
  else {
    gfx_rrect(s, FQ_X, FQ_Y, FQ_W, FQ_H, 14, C_PANEL, 255, 0);
    draw_spinner(ui, W / 2, FQ_Y + FQ_H / 2 - 30);
    text_draw_in(s, ui->fb, 26, FQ_X, FQ_W, FQ_Y + FQ_H / 2 + 40, "Carregando...", C_MUTED, 255, 0, 1);
    ui->dirty = 1;
  }
  gfx_rrect(s, FQ_X - 3, FQ_Y - 3, FQ_W + 6, FQ_H + 6, 16, C_EDGE, 255, 3);
  /* L1 / R1 nas laterais */
  gfx_rrect(s, FQ_X - 62, FQ_Y + FQ_H / 2 - 32, 46, 64, 10, C_PANEL2, 255, 0);
  text_draw_in(s, ui->fb, 18, FQ_X - 62, 46, FQ_Y + FQ_H / 2 + 7, "L1", C_TEXT, 255, 0, 1);
  gfx_rrect(s, FQ_X + FQ_W + 16, FQ_Y + FQ_H / 2 - 32, 46, 64, 10, C_PANEL2, 255, 0);
  text_draw_in(s, ui->fb, 18, FQ_X + FQ_W + 16, 46, FQ_Y + FQ_H / 2 + 7, "R1", C_TEXT, 255, 0, 1);
  /* barra de tempo e bolinhas */
  int by = FQ_Y + FQ_H + 6;
  gfx_fill(s, FQ_X, by, FQ_W, 6, C_PANEL2, 255);
  if (!ui->fq_pause && im) gfx_fill(s, FQ_X, by, (int)((long)FQ_W * ui->fq_t / FQ_TICKS), 6, C_FOCUS, 255);
  int dx = W / 2 - (n * 26) / 2;
  for (int k = 0; k < n; k++) gfx_circle(s, dx + k * 26 + 13, by + 26, k == ui->fq_i ? 7 : 5, k == ui->fq_i ? C_FOCUS : C_MUTED, k == ui->fq_i ? 255 : 140);
}

static void fq_footer(UI *ui, int *px) {
  int x = *px;
  x = hint(ui, x, "L1  R1", "Anterior / Próxima");
  x = hint(ui, x, "X", ui->cat->faq[ui->fq_i].video[0] ? "Ver vídeo" : (ui->fq_pause ? "Continuar" : "Pausar"));
  x = hint(ui, x, "O", "Voltar");
  if (update_available(ui)) x = hint(ui, x, "\xE2\x96\xA1", "Atualizar app");
  *px = x;
}
