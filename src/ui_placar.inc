/* ------------------------------------------------------------------ v1.37: tela JK-MOD PLACARES
 * Card com tag PLACARGRID. Os dados vem das linhas PLB/PLO/PLP do catalogo (ver catalog.h).
 * Grade 3x2: cada card = 1 combo (Serie A, Serie B, Copa do Brasil, Libertadores) com a imagem do placar e o logo.
 * O ultimo card e o "Monte o seu": escolhe 1 placar por competicao.
 * Controle: direcional move, X baixa / monta, O volta, L1/R1 troca o patch (Demisan / Mega...).
 * Incluido no ui.c (usa a struct UI e as funcoes de desenho de la). */

#define PL_X0   80
#define PL_Y0   186            /* v1.40: abaixo da barra de abas */
#define PL_CW   568
#define PL_CH   398
#define PL_GX   28
#define PL_GY   16
#define PL_PAD  14
#define PL_IW   468            /* caixa da imagem do placar no card */
#define PL_IH   64
#define PL_LW   64             /* caixa do logo no card */
#define PL_RS   80             /* distancia entre as linhas de competicao no card */
#define PL_EIW  900            /* editor (Monte o seu) */
#define PL_EIH  110
#define PL_ELW  116
#define PL_MAXU (CAT_MAX_PLO * 2 + CAT_MAX_ART + CAT_MAX_JKX)   /* v1.43: + cards dos plugins */   /* v1.40: + cards de narrador/clube (logo = 2: imagem pronta, sem escala) */
#define PL_PER  6              /* cards por pagina */

#define C_DEEP  RGB(0x0b, 0x12, 0x22)
#define C_EDGE  RGB(0x22, 0x30, 0x4b)

static const char *PL_COMP[8] = { "BRASILEIRÃO SÉRIE A", "BRASILEIRÃO SÉRIE B", "COPA DO BRASIL", "LIBERTADORES", "PLACAR MUNDIAL", "SUL-AMERICANA", "SUPERCOPA REI", "RECOPA" };
static const char *PL_FILE[8] = { "A", "B", "CDB", "LIB", "MUN", "SULA", "SUPER", "RECOPA" };
/* v1.49: ordem das linhas no "Monte o seu" (Placar Mundial por ultimo) e aviso amarelo de cada uma */
static const int PL_ORD[8] = { 0, 1, 2, 3, 5, 6, 7, 4 };
static const char *PL_NOTE[8] = { "", "", "", "", "outras ligas e amistosos", "precisa do plugin Sula", "precisa do plugin Sula", "precisa do plugin Sula" };
#define C_YELLOW RGB(0xff, 0xd2, 0x3c)
static int pl_count(const CatPlBoard *b, int c);
/* v1.43: tamanhos das imagens (menores quando o quadro tem a 5a linha) */
static int pli_ih = PL_IH, pli_lw = PL_LW, pli_eih = PL_EIH, pli_elw = PL_ELW;
static int pli_nobox = 0;                                        /* v1.49: 7/8 linhas: placar sem a caixa escura (preenche a linha) */

static int is_plgrid(const CatGame *g) { return g && !strcmp(g->tag, "PLACARGRID"); }
static int pl_active(UI *ui) { return ui->mode == MODE_CATALOG && ui->detail && is_plgrid(cur_game(ui)); }

static const CatPlBoard *pl_board_cur(UI *ui) {
  if (!ui->cat || ui->cat->nplb <= 0) return NULL;
  if (ui->pl_b < 0 || ui->pl_b >= ui->cat->nplb) ui->pl_b = 0;
  return &ui->cat->plb[ui->pl_b];
}
static int pl_ncards(const CatPlBoard *b) { return b ? b->npack + (b->monte ? 1 : 0) : 0; }
/* v1.43: 4 competicoes, ou 5 se o catalogo tiver placares MUN para o quadro */
static int pl_rows(const CatPlBoard *b, int *out) {              /* v1.49: competicoes mostradas, na ordem de tela (as 4 fixas + as opcionais que tem placar) */
  int n = 0;
  for (int k = 0; k < 8; k++) { int c = PL_ORD[k]; if (c < 4 || (b && pl_count(b, c) > 0)) out[n++] = c; }
  return n;
}
static int pl_ncomp(const CatPlBoard *b) { int r[8]; return pl_rows(b, r); }
static int pl_row_comp(const CatPlBoard *b, int row) { int r[8], n = pl_rows(b, r); return row >= 0 && row < n ? r[row] : 0; }

/* ---- imagens (baixadas em segundo plano, guardadas na mesma pasta das capas) */
static pthread_mutex_t pl_mu = PTHREAD_MUTEX_INITIALIZER;
static int plr_gen = 0, plr_pending = 0;
static Image *plr_im[PL_MAXU][2];
static int plr_rdy[PL_MAXU];

typedef struct { UI *ui; int gen, n; char url[PL_MAXU][256]; int logo[PL_MAXU]; int ih, lw, eih, elw; } PlJobs;

static Image *pl_fit(const Image *src, int bw, int bh) {
  if (!src || src->w <= 0 || src->h <= 0) return NULL;
  double sx = (double)bw / src->w, sy = (double)bh / src->h, sc = sx < sy ? sx : sy;
  int w = (int)(src->w * sc + 0.5), h = (int)(src->h * sc + 0.5);
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  return img_scale(src, w, h);
}

/* v1.43: corta a borda transparente da imagem do placar (a barra ocupa o espaco todo do card) */
static Image *pl_trim(Image *im) {
  if (!im) return im;
  int x0 = im->w, y0 = im->h, x1 = -1, y1 = -1;
  for (int y = 0; y < im->h; y++)
    for (int x = 0; x < im->w; x++)
      if ((im->px[y * im->w + x] >> 24) > 16) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
  if (x1 < 0 || (x0 == 0 && y0 == 0 && x1 == im->w - 1 && y1 == im->h - 1)) return im;
  Image *o = img_new(x1 - x0 + 1, y1 - y0 + 1);
  if (!o) return im;
  for (int y = 0; y < o->h; y++) memcpy(o->px + y * o->w, im->px + (y0 + y) * im->w + x0, (size_t)o->w * 4);
  img_free(im);
  return o;
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
    if (J->logo[k] == 2) { a = raw; b = NULL; raw = NULL; }
    else if (J->logo[k]) { a = pl_fit(raw, J->lw - 6, J->lw - 6); b = pl_fit(raw, J->elw - 14, J->elw - 14); }
    else            { raw = pl_trim(raw); a = pl_fit(raw, PL_IW - 10, J->ih - 6);  b = pl_fit(raw, PL_EIW - 16, J->eih >= 74 ? J->eih - 2 : J->eih - 8);   /* v1.50: 7/8 linhas: placar quase da altura da linha */ }
    if (raw) img_free(raw);
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
  for (int i = 0; i < CAT_MAX_ART; i++) ui->jk_ai[i] = -1;
  for (int i = 0; i < CAT_MAX_JKX; i++) ui->jx_ai[i] = ui->jx_av[i] = -1;
  if (!b) return;
  PlJobs *J = (PlJobs *)calloc(1, sizeof(PlJobs));
  if (!J) return;
  J->ui = ui; J->gen = gen;
  { int nc_ = pl_ncomp(b), five = nc_ >= 5;                      /* v1.43; v1.45: 6 linhas; v1.49: 7 e 8 */
    pli_ih = PL_IH; pli_lw = PL_LW; pli_eih = nc_ >= 7 ? 74 : nc_ >= 6 ? 70 : five ? 84 : PL_EIH; pli_elw = nc_ >= 7 ? 74 : nc_ >= 6 ? 70 : five ? 84 : PL_ELW;   /* card do combo: sempre 4 linhas (barra grande) */
    J->ih = pli_ih; J->lw = pli_lw; J->eih = pli_eih; J->elw = pli_elw; }
  for (int i = 0; i < b->nopt; i++) {                            /* primeiro as imagens dos placares, depois os logos */
    ui->pl_oi[i] = pl_url_add(ui, J, b->opt[i].img, 0);
  }
  for (int i = 0; i < b->nopt; i++) ui->pl_ol[i] = pl_url_add(ui, J, b->opt[i].logo, 1);
  for (int i = 0; i < ui->cat->nart && i < CAT_MAX_ART; i++) ui->jk_ai[i] = pl_url_add(ui, J, ui->cat->art[i].img, 2);   /* v1.40 */
  for (int i = 0; i < ui->cat->njkx && i < CAT_MAX_JKX; i++) ui->jx_ai[i] = pl_url_add(ui, J, ui->cat->jkx[i].img, 2);   /* v1.43 */
  for (int i = 0; i < ui->cat->njkx && i < CAT_MAX_JKX; i++) ui->jx_av[i] = pl_url_add(ui, J, jkx_aviso(&ui->cat->jkx[i]), 2);   /* v1.44: imagem inteira, sem escala */
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
  for (int c = 0; c < CAT_PL_COMPS; c++) ui->pl_inst[c] = -1;
  if (!b || !b->dest[0]) return;
  for (int c = 0; c < CAT_PL_COMPS; c++) {
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
  for (int c = 4; c < CAT_PL_COMPS; c++) if (p->opt[c] >= 0 && ui->pl_inst[c] != p->opt[c]) return 0;   /* v1.43 */
  return 1;
}


/* ------------------------------------------------------------------ v1.40: abas NARRADORES e MASTER LEAGUE PRO
 * Os cards (imagem pronta: foto/escudo, logo da emissora, nome) vem das linhas JKN/JKC. Os downloads sao os botoes que ja
 * existem no card JK-MOD de cada patch (linha JKG): narrador = botao sem tipo (ORIGINAL) e COL3 (ESTRANGEIRA); clube = COL4.
 * O botao e achado pelo nome (catalog_key). Instalado = o arquivo do botao esta no console com o mesmo tamanho. */
static void pl_blit_center(UI *ui, const Image *im, int x, int y, int w, int h);
static void pl_glow(Image *s, int x, int y, int w, int h, int r, uint32_t col);
static const char *JK_TABS[4] = { "PLACARES", "NARRADORES", "MASTER LEAGUE PRO", "PLUGINS" };
/* v1.43: a aba PLUGINS so aparece se o catalogo tiver linhas JKX */
static int jk_ntabs(UI *ui) { return (ui->cat && ui->cat->njkx > 0) ? 4 : 3; }
static int jk_grid(UI *ui) { return ui->pl_cat == 1 || ui->pl_cat == 3; }       /* grade de cards 280x250 com 2 botoes */
#define JK_Y0    192
#define JK_NCW   280
#define JK_NCH   250
#define JK_NCOLS 6
#define JK_NGX   16
#define JK_NGY   14
#define JK_MCW   340
#define JK_MCH   184
#define JK_MCOLS 5
#define JK_MGX   14
#define JK_MGY   12
#define C_CYAN   RGB(0x78, 0xdc, 0xff)
/* v1.43d: aba PLUGINS: cards mais altos (titulo / arquivo / pasta) em 7 colunas, 2 linhas visiveis com rolagem */
#define JX_CW    256
#define JX_CH    300
#define JX_COLS  7
#define JX_GX    12
#define JX_GY    12
#define JX_ROWS  2
static void jk_dims(UI *ui, int *cols, int *cw, int *ch, int *gx, int *gy) {
  int narr = jk_grid(ui), px = ui->pl_cat == 3;
  *cols = px ? JX_COLS : narr ? JK_NCOLS : JK_MCOLS;
  *cw = px ? JX_CW : narr ? JK_NCW : JK_MCW;
  *ch = px ? JX_CH : narr ? JK_NCH : JK_MCH;
  *gx = px ? JX_GX : narr ? JK_NGX : JK_MGX;
  *gy = px ? JX_GY : narr ? JK_NGY : JK_MGY;
}

static const CatGame *jk_find_game(UI *ui) {
  const CatPlBoard *b = pl_board_cur(ui);
  if (!b || !ui->cat) return NULL;
  for (int i = 0; i < ui->cat->njk; i++) {
    if (strcmp(ui->cat->jk[i].board, b->id)) continue;
    for (int g = 0; g < ui->cat->ngame; g++) if (!strcmp(ui->cat->game[g].title, ui->cat->jk[i].game)) return &ui->cat->game[g];
  }
  return NULL;
}

static void jk_fix_btn(UI *ui) {
  if (!jk_grid(ui) || ui->jk_n <= 0) { ui->jk_btn = 0; return; }
  int s = ui->jk_sel;
  if (ui->jk_btn < 0 || ui->jk_btn > 1) ui->jk_btn = 0;
  if (ui->jk_b[s][ui->jk_btn] < 0) ui->jk_btn = !ui->jk_btn;
}

static void jk_build(UI *ui) {
  ui->jk_n = 0; ui->jk_scanned = 0; ui->jx_arm = 0; ui->jx_top = 0; ui->jx_modal = 0;
  ui->jk_game = jk_find_game(ui);
  const CatGame *g = ui->jk_game;
  if (ui->pl_cat == 3) {                                         /* v1.43: plugins do quadro (uma card por chave) */
    const CatPlBoard *b = pl_board_cur(ui);
    for (int i = 0; b && i < ui->cat->njkx && ui->jk_n < CAT_MAX_ART; i++) {
      const CatJkx *x = &ui->cat->jkx[i];
      if (!jkx_for_board(x, b->id)) continue;
      int dup = 0;
      for (int k = 0; k < ui->jk_n; k++) if (!strcmp(ui->cat->jkx[ui->jk_art[k]].key, x->key)) dup = 1;
      if (dup) continue;
      int n = ui->jk_n++;
      ui->jk_art[n] = i; ui->jk_b[n][0] = 0; ui->jk_b[n][1] = -1; ui->jk_inst[n] = 0;
    }
  } else if (g && ui->pl_cat > 0) {
    int kind = ui->pl_cat == 1 ? 0 : 1;
    for (int a = 0; a < ui->cat->nart && ui->jk_n < CAT_MAX_ART; a++) {
      const CatArt *art = &ui->cat->art[a];
      if (art->kind != kind) continue;
      int bo = -1, be = -1;
      for (int i = 0; i < g->nbtn; i++) {
        const CatButton *bt = &g->btn[i];
        if (strcmp(bt->action, "download") || !bt->url[0]) continue;
        char k[48];
        catalog_key(bt->label, k, sizeof(k));
        if (strcmp(k, art->key)) continue;
        if (kind == 1) { if (!strcmp(bt->type, "COL4") && bo < 0) bo = i; }
        else if (!strcmp(bt->type, "COL3")) { if (be < 0) be = i; }
        else if (!bt->type[0] && bo < 0) bo = i;
      }
      if (bo < 0 && be < 0) continue;
      int n = ui->jk_n++;
      ui->jk_art[n] = a; ui->jk_b[n][0] = bo; ui->jk_b[n][1] = be; ui->jk_inst[n] = 0;
    }
  }
  if (ui->jk_sel >= ui->jk_n) ui->jk_sel = ui->jk_n > 0 ? ui->jk_n - 1 : 0;
  if (ui->jk_sel < 0) ui->jk_sel = 0;
  jk_fix_btn(ui);
  ui->dirty = 1;
}

/* pasta destino + nome do arquivo do link (%20 -> espaco) */
static void jk_btn_path(const CatButton *b, char *out, int cap) {
  const char *u = b->url, *q = strrchr(u, '/');
  const char *name = q ? q + 1 : u;
  char nm[160]; int n = 0;
  for (const char *p = name; *p && *p != '?' && n < (int)sizeof(nm) - 1; p++) {
    if (p[0] == '%' && p[1] == '2' && p[2] == '0') { nm[n++] = ' '; p += 2; }
    else nm[n++] = *p;
  }
  nm[n] = 0;
  snprintf(out, (size_t)cap, "%s/%s", b->dest[0] ? b->dest : "/data", nm);
}

static void jk_scan(UI *ui) {
  ui->jk_scanned = 1;
  if (ui->pl_cat == 3) {                                         /* v1.43 */
    const CatPlBoard *b = pl_board_cur(ui);
    for (int n = 0; n < ui->jk_n; n++) {
      const CatJkx *xx = &ui->cat->jkx[ui->jk_art[n]];
      ui->jx_path[n][0] = 0; ui->jx_file[n][0] = 0;
      ui->jx_tog[n] = jkx_toggleable(xx) && !jkx_fixed_note(xx);   /* v1.47 */
      ui->jx_on[n] = 1;
      if (jkx_fixed(xx) && !strcmp(xx->files, "FIXO")) {        /* v1.43d: card FIXO so com prx (vem com o placar) */
        ui->jk_inst[n] = b ? jkx_fixed_present(xx, b) : 0; ui->jk_b[n][1] = -1;   /* v1.47: no console (ligado ou nao) */
        ui->jx_on[n] = b ? jkx_fixed_active(xx, b) : 0;
        char f[64]; int k = 0;
        const char *s0 = xx->prx, *e0 = strchr(s0, '|'); if (!e0) e0 = s0 + strlen(s0);
        const char *nm = s0; for (const char *q = s0; q < e0; q++) if (*q == '/') nm = q + 1;
        while (nm < e0 && k < 63) f[k++] = *nm++;
        f[k] = 0;
        text_fit(ui->fb, 15, f, JX_CW - 20, ui->jx_file[n], sizeof(ui->jx_file[n]));
        text_fit(ui->fb, 13, "GoldHEN/plugins", JX_CW - 20, ui->jx_path[n], sizeof(ui->jx_path[n]));
        continue;
      }
      int fx = jkx_fixed(xx);                                    /* v1.43e: FIXO com arquivos: mostra arquivo/pasta, sem botoes */
      int in = b ? (fx ? jkx_fixed_present(xx, b) : jkx_installed(xx, b)) : 0;
      ui->jk_inst[n] = in; ui->jk_b[n][1] = (in && !fx) ? 0 : -1;   /* v1.47: AVISO saiu do botao (agora e o triangulo) */
      ui->jx_on[n] = !b ? 0 : fx ? jkx_fixed_active(xx, b) : jkx_enabled(xx, b);
      if (b) {                                                   /* pastas para onde vai: "GoldHEN/plugins + PCC59" */
        static char ps[8][200];
        int np = jkx_paths(&ui->cat->jkx[ui->jk_art[n]], b, ps, 8);
        if (np > 0) {
          char t[200] = "";
          size_t L = 0;
          for (int k = 0; k < np; k++) {
            char *sl = strrchr(ps[k], '/'); if (sl) *sl = 0;        /* tira o nome do arquivo */
            const char *q = !strncmp(ps[k], "/data/", 6) ? ps[k] + 6 : ps[k];
            int dup = 0;
            for (int j = 0; j < k; j++) { const char *r = !strncmp(ps[j], "/data/", 6) ? ps[j] + 6 : ps[j]; if (!strcmp(r, q)) dup = 1; }
            if (dup) continue;
            L += (size_t)snprintf(t + L, sizeof(t) - L, "%s%s", L ? " + " : "", q);
            if (L >= sizeof(t) - 1) break;
          }
          text_fit(ui->fb, 13, t, JX_CW - 20, ui->jx_path[n], sizeof(ui->jx_path[n]));
          /* nome do arquivo principal: os .prx e .cpk do plugin (os outros sao de apoio) */
          char f[200] = ""; size_t F = 0; int lista = 0;
          np = jkx_paths(xx, b, ps, 8);                          /* de novo: o laco acima cortou o nome */
          for (int k = 0; k < np; k++) {
            const char *nm = strrchr(ps[k], '/'); nm = nm ? nm + 1 : ps[k];
            size_t L2 = strlen(nm);
            if (!strcmp(nm, "x")) { lista = 1; continue; }
            char ex[5] = ""; for (int e = 0; L2 > 4 && e < 4; e++) { char c = nm[L2 - 4 + e]; ex[e] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
            int main_ = !strcmp(ex, ".prx") || !strcmp(ex, ".cpk");
            if (!main_) continue;
            F += (size_t)snprintf(f + F, sizeof(f) - F, "%s%s", F ? " + " : "", nm);
            if (F >= sizeof(f) - 1) break;
          }
          if (lista && F < sizeof(f) - 12) F += (size_t)snprintf(f + F, sizeof(f) - F, "%s", F ? " + hinos" : "hinos");
          if (!F && np > 0) { const char *nm = strrchr(ps[0], '/'); snprintf(f, sizeof(f), "%s", nm ? nm + 1 : ps[0]); }
          text_fit(ui->fb, 15, f, JX_CW - 20, ui->jx_file[n], sizeof(ui->jx_file[n]));
        }
      }
    }
    jk_fix_btn(ui);
    return;
  }
  const CatGame *g = ui->jk_game;
  if (!g) return;
  for (int n = 0; n < ui->jk_n; n++) {
    ui->jk_inst[n] = 0;
    for (int k = 0; k < 2; k++) {
      int bi = ui->jk_b[n][k];
      if (bi < 0 || !g->btn[bi].size) continue;
      char p[400];
      jk_btn_path(&g->btn[bi], p, sizeof(p));
      if (pl_fsize(p) == (long long)g->btn[bi].size) ui->jk_inst[n] = k + 1;
    }
  }
}

static void jkx_go(UI *ui) {                                     /* v1.43: X na aba PLUGINS */
  const CatPlBoard *b = pl_board_cur(ui);
  if (!b || ui->jk_n <= 0) return;
  int sel = ui->jk_sel;
  const CatJkx *x = &ui->cat->jkx[ui->jk_art[sel]];
  if (jkx_fixed_note(x)) { char t[160]; snprintf(t, sizeof(t), "%s: %s", x->name, jkx_fixed_note(x)); ui_toast(ui, t, UI_TOAST_INFO); return; }   /* v1.43g */
  int fx = jkx_fixed(x), in = ui->jk_inst[sel];
  if (in && ui->jx_tog[sel] && (fx || ui->jk_btn == 0)) {        /* v1.47: ATIVADO <-> DESATIVADO (so o plugins.ini / DpFileList) */
    if (ui->busy) { ui_toast(ui, "Aguarde o download terminar para ativar ou desativar.", UI_TOAST_INFO); return; }
    int on = !ui->jx_on[sel];
    jkx_toggle(x, b, on, NULL);
    char t[160]; snprintf(t, sizeof(t), "%s %s. Feche e abra o PES para valer.", x->name, on ? "ATIVADO" : "DESATIVADO");
    ui_toast(ui, t, on ? UI_TOAST_OK : UI_TOAST_INFO);
    ui->jk_scanned = 0; ui->dirty = 1;
    return;
  }
  if (fx && (in || !jkx_fixed_files(x, b))) {                    /* v1.43d */
    ui_toast(ui, in ? "Vem junto com o JK-MOD e já está ativado." : "Vem junto com o JK-MOD: baixe um placar, narração ou Master League.", UI_TOAST_INFO);
    return;
  }
  if (ui->busy) { ui_toast(ui, "Já existe um pedido em andamento. Aguarde.", UI_TOAST_INFO); return; }
  static CatButton cb;
  if (ui->jk_btn == 1 && !fx) {
    if (!ui->jk_inst[sel]) return;
    if (ui->jx_arm != sel + 1 || ui->frame - ui->jx_arm_f > 240) {          /* 1o toque: so avisa */
      char t[160]; snprintf(t, sizeof(t), "Aperte X de novo para desinstalar: %s", x->name);
      ui_toast(ui, t, UI_TOAST_INFO);
      ui->jx_arm = sel + 1; ui->jx_arm_f = ui->frame;
      return;
    }
    ui->jx_arm = 0;
    jkx_button_off(x, b, &cb);
    ui_toast(ui, "Desinstalando o plugin...", UI_TOAST_INFO);
  } else {
    if (ui->jk_inst[sel]) { ui_toast(ui, "Este plugin já está instalado.", UI_TOAST_INFO); return; }
    if (ui->locked) {
      ui_toast(ui, ui->offline ? "Sem conexão com o servidor. Downloads indisponíveis agora." : "Ative seu plano para baixar.", UI_TOAST_ERR);
      return;
    }
    if (jkx_button(x, b, &cb) != 0) { ui_toast(ui, "Plugin sem arquivos no catálogo.", UI_TOAST_ERR); return; }
    ui_toast(ui, "Baixando o plugin para o PS4...", UI_TOAST_INFO);
  }
  ui->busy = 1;
  ui->updating = 0;
  ui->jk_scanned = 0;
  if (ui->fn) ui->fn(&cb, ui->user);
}

/* v1.47: triangulo na aba PLUGINS: video em tela cheia (se o card tem) ou o AVISO */
static void jkx_tri(UI *ui) {
  if (ui->jk_n <= 0) return;
  const CatJkx *x = &ui->cat->jkx[ui->jk_art[ui->jk_sel]];
  if (x->video[0]) {
    if (ui->vid_mode == 3 && !strcmp(ui->vurl, x->video) && vp_state() != VP_ERROR) {   /* o do card vira a janela, sem recomecar */
      snprintf(ui->vtitle, sizeof(ui->vtitle), "%s", x->name);
      vp_set_mute(0); ui->vid_mode = 1; ui->base_stale = 1; ui->dirty = 1;
      return;
    }
    vid_close(ui);
    ui->jx_vfail = -1;
    vid_open_url_ex(ui, x->name, x->video, 1);
    return;
  }
  if (jkx_aviso(x)) {
    int ai = ui->jx_av[ui->jk_art[ui->jk_sel]];
    if (ai < 0 || !ui->pl_im[ai][0]) { ui_toast(ui, "Carregando o aviso... tente de novo em instantes.", UI_TOAST_INFO); return; }
    ui->jx_modal = ui->jk_art[ui->jk_sel] + 1; ui->dirty = 1;
  }
}

/* v1.47: video no card selecionado (sem som). Comeca depois de ~1 s parado no card; sai do card = para. */
static void jx_card_video_tick(UI *ui) {
  int want = -1;
  if (ui->detail && ui->pl_cat == 3 && !ui->pl_ed && !ui->jx_modal && !ui->busy && ui->jk_n > 0 && ui->jk_sel < ui->jk_n) {
    int a = ui->jk_art[ui->jk_sel];
    if (ui->cat->jkx[a].video[0]) want = a;
  }
  if (ui->vid_mode == 1 || ui->vid_mode == 2) { ui->jx_vfoc = ui->frame; return; }   /* janela aberta: ela manda */
  if (ui->vid_mode == 3 && want != ui->jx_vart) { vid_close(ui); ui->jx_vart = -1; }
  if (want < 0) { ui->jx_vfoc = ui->frame; return; }
  if (ui->vid_mode == 3) {
    if (vp_state() == VP_ERROR) { ui->jx_vfail = want; vid_close(ui); ui->jx_vart = -1; }
    return;
  }
  if (want == ui->jx_vfail || ui->frame - ui->jx_vfoc < 50 || vp_busy()) return;
  vid_open_url_ex(ui, ui->cat->jkx[want].name, ui->cat->jkx[want].video, 1);
  if (ui->vid_mode == 1) { ui->vid_mode = 3; vp_set_mute(1); ui->jx_vart = want; ui->base_stale = 1; }
}

/* v1.47: retangulo do video no card (topo do card, 16:9). 0 = card fora da tela */
static void jk_pos(UI *ui, int i, int *px, int *py);
static int jx_video_rect(UI *ui, int *x, int *y, int *w, int *h) {
  if (ui->pl_cat != 3 || ui->jk_n <= 0) return 0;
  int cx, cy; jk_pos(ui, ui->jk_sel, &cx, &cy);
  if (cy < JK_Y0 || cy + JX_CH > 1000) return 0;
  *x = cx + 6; *y = cy + 6; *w = JX_CW - 12; *h = (JX_CW - 12) * 9 / 16;
  return 1;
}

static void jk_install(UI *ui) {
  if (ui->pl_cat == 3) { jkx_go(ui); return; }
  const CatGame *g = ui->jk_game;
  if (!g || ui->jk_n <= 0) return;
  int bi = ui->jk_b[ui->jk_sel][ui->pl_cat == 1 ? ui->jk_btn : 0];
  if (bi < 0) { ui_toast(ui, "Esta versão não existe para este patch.", UI_TOAST_INFO); return; }
  const CatButton *b = &g->btn[bi];
  if (ui->locked) {
    ui_toast(ui, ui->offline ? "Sem conexão com o servidor. Downloads indisponíveis agora." : "Ative seu plano para baixar.", UI_TOAST_ERR);
    return;
  }
  if (ui->busy) { ui_toast(ui, "Já existe um pedido em andamento. Aguarde.", UI_TOAST_INFO); return; }
  ui_toast(ui, ui->pl_cat == 1 ? "Baixando a narração para o PS4..." : "Baixando a Master League para o PS4...", UI_TOAST_INFO);
  ui->busy = 1;
  ui->updating = 0;
  ui->jk_scanned = 0;
  if (ui->fn) ui->fn(b, ui->user);
}

/* posicao do item i na grade (a ultima linha fica centralizada) */
static void jk_pos(UI *ui, int i, int *px, int *py) {
  int cols, cw, ch, gx, gy;
  jk_dims(ui, &cols, &cw, &ch, &gx, &gy);
  int r = i / cols, c = i % cols, inrow = ui->jk_n - r * cols;
  if (inrow > cols) inrow = cols;
  *px = (W - (inrow * cw + (inrow - 1) * gx)) / 2 + c * (cw + gx);
  *py = JK_Y0 + (r - (ui->pl_cat == 3 ? ui->jx_top : 0)) * (ch + gy);
}

static void jk_key(UI *ui, int key) {
  int n = ui->jk_n, sel = ui->jk_sel, narr = jk_grid(ui);
  int cols, cw_, ch_, gx_, gy_;
  jk_dims(ui, &cols, &cw_, &ch_, &gx_, &gy_);
  (void)cw_; (void)ch_; (void)gx_; (void)gy_;
  int col = sel % cols;
  switch (key) {
    case UI_LEFT:
      if (narr && ui->jk_btn == 1 && n > 0 && ui->jk_b[sel][0] >= 0) { ui->jk_btn = 0; ui->dirty = 1; return; }
      if (col > 0) { sel--; ui->jk_btn = 1; }
      break;
    case UI_RIGHT:
      if (narr && ui->jk_btn == 0 && n > 0 && ui->jk_b[sel][1] >= 0) { ui->jk_btn = 1; ui->dirty = 1; return; }
      if (col < cols - 1 && sel + 1 < n) { sel++; ui->jk_btn = 0; }
      break;
    case UI_UP:   if (sel - cols >= 0) sel -= cols; break;
    case UI_DOWN:
      if (sel + cols < n) sel += cols;
      else if ((sel / cols + 1) * cols < n) sel = n - 1;
      break;
    case UI_L1: case UI_R1:
      if (ui->cat && ui->cat->nplb > 1) {
        int nb = ui->cat->nplb;
        ui->pl_b = (ui->pl_b + (key == UI_R1 ? 1 : nb - 1)) % nb;
        pl_open(ui);
      }
      return;
    case UI_OK: jk_install(ui); return;
    case UI_BACK: ui->detail = 0; ui->pl_ed = 0; ui->dirty = 1; rebuild_fbg(ui); return;
    case UI_SQUARE: trigger_update(ui); return;
    case UI_TRIANGLE: if (ui->pl_cat == 3) jkx_tri(ui); return;   /* v1.47 */
    default: return;
  }
  if (n > 0 && sel >= 0 && sel < n) {
    if (sel != ui->jk_sel) ui->jx_vfoc = ui->frame;              /* v1.47: video do card espera de novo */
    ui->jk_sel = sel; jk_fix_btn(ui); ui->dirty = 1;
    if (ui->pl_cat == 3) {                                       /* v1.43d: rola para a linha do item */
      int r = sel / cols;
      if (r < ui->jx_top) ui->jx_top = r;
      if (r >= ui->jx_top + JX_ROWS) ui->jx_top = r - JX_ROWS + 1;
    }
  }
}

static void jk_tabs(UI *ui) {
  Image *s = ui->s;
  gfx_fill(s, 0, 116, W, 60, RGB(0x0a, 0x10, 0x1e), 230);
  gfx_fill(s, 0, 176, W, 1, C_EDGE, 255);
  int nt = jk_ntabs(ui), fs = 24, gap = 16, ws[4], tot = 0;
  for (int i = 0; i < nt; i++) { ws[i] = TW(ui, 1, fs, JK_TABS[i], 2) + 80; tot += ws[i]; }
  tot += gap * (nt - 1);
  int x = (W - tot) / 2;
  gfx_rrect(s, x - 70, 129, 50, 34, 8, C_EDGE, 255, 2); text_draw_in(s, ui->fb, 17, x - 70, 50, 152, "L2", C_MUTED, 255, 0, 1);
  for (int i = 0; i < nt; i++) {
    int on = i == ui->pl_cat;
    if (on) { gfx_rrect(s, x, 124, ws[i], 44, 12, C_PANEL2, 255, 0); gfx_fill(s, x + 14, 164, ws[i] - 28, 4, C_FOCUS, 255); }
    text_draw_in(s, ui->fb, fs, x, ws[i], 155, JK_TABS[i], on ? C_FOCUS : C_MUTED, 255, 2, 1);
    x += ws[i] + gap;
  }
  gfx_rrect(s, x + 4, 129, 50, 34, 8, C_EDGE, 255, 2); text_draw_in(s, ui->fb, 17, x + 4, 50, 152, "R2", C_MUTED, 255, 0, 1);
}

/* botao pequeno do card do narrador. st: 0 normal, 1 em foco, 2 ativo (instalado), -1 nao existe */
static void jk_pill(UI *ui, int x, int y, int w, int h, const char *txt, int st) {
  Image *s = ui->s;
  if (st < 0) {
    gfx_rrect(s, x, y, w, h, 8, RGB(0x10, 0x18, 0x26), 255, 0);
    gfx_rrect(s, x, y, w, h, 8, C_EDGE, 255, 1);
    text_draw_in(s, ui->fb, 14, x, w, y + h / 2 + 5, txt, C_MUTED, 90, 0, 1);
    return;
  }
  uint32_t bg = st == 1 ? C_FOCUS : (st == 2 ? C_BASE : RGB(0x0c, 0x1e, 0x34));
  gfx_rrect(s, x, y, w, h, 8, bg, 255, 0);
  if (st == 2) gfx_rrect(s, x, y, w, h, 8, C_BASE_EDGE, 255, 2);
  else if (st == 0) gfx_rrect(s, x, y, w, h, 8, C_CYAN, 255, 1);
  text_draw_in(s, ui->fb, 14, x, w, y + h / 2 + 5, st == 2 ? "\xE2\x9C\x93 ATIVO" : txt, st == 1 ? C_BG : C_TEXT, 255, 0, 1);
}

/* v1.47: ATIVADO (verde) / DESATIVADO (vermelho). foc = em foco */
static void jx_toggle_pill(UI *ui, int x, int y, int w, int h, int on, int foc) {
  Image *s = ui->s;
  uint32_t edge = on ? RGB(0x3c, 0xd2, 0x78) : RGB(0xe6, 0x5a, 0x64);
  uint32_t bg = foc ? edge : on ? RGB(0x0e, 0x3a, 0x22) : RGB(0x46, 0x14, 0x1c);
  gfx_rrect(s, x, y, w, h, 8, bg, 255, 0);
  gfx_rrect(s, x, y, w, h, 8, edge, 255, foc ? 0 : 2);
  int fs = w < 118 ? 12 : 15;
  text_draw_in(s, ui->fb, fs, x, w, y + h / 2 + (fs == 12 ? 4 : 5), on ? "\xE2\x9C\x93 ATIVADO" : "\xE2\x9C\x97 DESATIVADO", foc ? C_BG : C_TEXT, 255, 0, 1);
}

/* v1.43: botoes do card de plugin. st: -1 apagado, 0 normal, 1 em foco, 2 instalado. red = DESINSTALAR */
static void jx_pill(UI *ui, int x, int y, int w, int h, const char *txt, int st, int red) {
  Image *s = ui->s;
  if (st < 0) {
    gfx_rrect(s, x, y, w, h, 8, RGB(0x10, 0x18, 0x26), 255, 0);
    gfx_rrect(s, x, y, w, h, 8, C_EDGE, 255, 1);
    text_draw_in(s, ui->fb, w < 118 ? 12 : 14, x, w, y + h / 2 + 4, txt, C_MUTED, 90, 0, 1);
    return;
  }
  if (red == 2) {                                                /* v1.44: AVISO (amarelo) */
    gfx_rrect(s, x, y, w, h, 8, st == 1 ? RGB(0xf5, 0xc5, 0x18) : RGB(0x3a, 0x2e, 0x08), 255, 0);
    if (st != 1) gfx_rrect(s, x, y, w, h, 8, RGB(0xf5, 0xc5, 0x18), 255, 1);
    int f2 = w < 118 ? 12 : 14;
    text_draw_in(s, ui->fb, f2, x, w, y + h / 2 + (f2 == 12 ? 4 : 5), txt, st == 1 ? C_BG : RGB(0xff, 0xe0, 0x70), 255, 0, 1);
    return;
  }
  uint32_t bg = st == 1 ? (red ? C_ERR : C_FOCUS) : st == 2 ? C_BASE : red ? RGB(0x46, 0x14, 0x1c) : RGB(0x0c, 0x1e, 0x34);
  gfx_rrect(s, x, y, w, h, 8, bg, 255, 0);
  if (st == 2) gfx_rrect(s, x, y, w, h, 8, C_BASE_EDGE, 255, 2);
  else if (st == 0) gfx_rrect(s, x, y, w, h, 8, red ? RGB(0xe6, 0x5a, 0x64) : C_CYAN, 255, 1);
  int fs = w < 118 ? 12 : 14;                                    /* v1.43d: card de 256 px */
  text_draw_in(s, ui->fb, fs, x, w, y + h / 2 + (fs == 12 ? 4 : 5), st == 2 ? "\xE2\x9C\x93 INSTALADO" : txt, st == 1 ? C_BG : C_TEXT, 255, 0, 1);
}

static void jk_placeholder(UI *ui, const CatGame *g, int n, int x, int y, int w, int h) {
  if (ui->pl_cat == 3) {                                         /* v1.43: plugin sem imagem: so o nome */
    Image *s = ui->s;
    gfx_rrect(s, x, y, w, h, 14, C_PANEL, 245, 0);
    gfx_rrect(s, x, y, w, h, 14, C_CYAN, 200, 2);
    char t[64]; text_fit(ui->fb, 22, ui->cat->jkx[ui->jk_art[n]].name, w - 30, t, sizeof(t));
    text_draw_in(s, ui->fb, 22, x, w, y + 170, t, C_TEXT, 255, 0, 1);
    return;
  }
  Image *s = ui->s;
  gfx_rrect(s, x, y, w, h, 14, C_PANEL, 245, 0);
  gfx_rrect(s, x, y, w, h, 14, C_CYAN, 200, 2);
  int bi = ui->jk_b[n][0] >= 0 ? ui->jk_b[n][0] : ui->jk_b[n][1];
  if (g && bi >= 0) {
    char t[64]; text_fit(ui->fb, 22, g->btn[bi].label, w - 30, t, sizeof(t));
    text_draw_in(s, ui->fb, 22, x, w, y + (ui->pl_cat == 1 ? 180 : h / 2 + 8), t, C_TEXT, 255, 0, 1);
  }
}

/* v1.44: AVISO do plugin (imagem do catalogo, tela cheia por cima da aba) */
static void jx_draw_modal(UI *ui) {
  if (!ui->jx_modal || ui->jx_modal > CAT_MAX_JKX) return;
  int ai = ui->jx_av[ui->jx_modal - 1];
  Image *im = ai >= 0 ? ui->pl_im[ai][0] : NULL;
  if (!im) { ui->jx_modal = 0; return; }
  Image *s = ui->s;
  gfx_rrect(s, 0, 0, W, H, 0, RGB(0, 0, 0), 205, 0);
  int x = (W - im->w) / 2, y = im->h <= 900 ? 34 : (H - 100 - im->h) / 2;
  gfx_rrect(s, x - 6, y - 6, im->w + 12, im->h + 12, 18, RGB(0xf5, 0xc5, 0x18), 255, 2);
  gfx_blit(s, im, x, y, 0, 255);
  text_draw_in(s, ui->fb, 20, 0, W, y + im->h + 38, "X  ou  O   fechar", C_TEXT, 255, 0, 1);
}

static void jk_draw(UI *ui) {
  Image *s = ui->s;
  const CatGame *g = ui->jk_game;
  if (ui->jk_n <= 0) {
    text_draw_in(s, ui->fb, 30, 0, W, 560, ui->pl_cat == 1 ? "Nenhum narrador para este patch." : ui->pl_cat == 3 ? "Nenhum plugin para este patch." : "Nenhuma Master League para este patch.", C_MUTED, 255, 0, 1);
    return;
  }
  int narr = jk_grid(ui), cols_, cw, ch, gx_, gy_;
  jk_dims(ui, &cols_, &cw, &ch, &gx_, &gy_);
  (void)cols_; (void)gx_; (void)gy_;
  for (int i = 0; i < ui->jk_n; i++) {
    int x, y, foc = i == ui->jk_sel;
    jk_pos(ui, i, &x, &y);
    if (ui->pl_cat == 3 && (y < JK_Y0 || y + ch > 1000)) continue;   /* v1.43d: fora da janela (rolagem) */
    int ai = ui->pl_cat == 3 ? ui->jx_ai[ui->jk_art[i]] : ui->jk_ai[ui->jk_art[i]];
    Image *im = ai >= 0 ? ui->pl_im[ai][0] : NULL;
    if (im) pl_blit_center(ui, im, x, y, cw, ch); else jk_placeholder(ui, g, i, x, y, cw, ch);
    if (ui->pl_cat == 3) {                                       /* v1.43 */
      int in = ui->jk_inst[i];
      if (ui->jx_file[i][0]) text_draw_in(s, ui->fb, 15, x, cw, y + 206, ui->jx_file[i], C_TEXT, 230, 0, 1);
      if (ui->jx_path[i][0]) text_draw_in(s, ui->fb, 13, x, cw, y + 226, ui->jx_path[i], C_CYAN, 210, 0, 1);
      const char *note = jkx_fixed_note(&ui->cat->jkx[ui->jk_art[i]]);
      if (note) {                                                /* v1.43g: aviso fixo (ex.: DESATIVADO - TRAVANDO) */
        char nt[64]; text_fit(ui->fb, 13, note, cw - 44, nt, sizeof(nt));
        gfx_rrect(s, x + 16, y + 246, cw - 32, 34, 8, foc ? C_ERR : RGB(0x46, 0x14, 0x1c), 255, 0);
        gfx_rrect(s, x + 16, y + 246, cw - 32, 34, 8, RGB(0xe6, 0x5a, 0x64), 255, 1);
        text_draw_in(s, ui->fb, 13, x + 16, cw - 32, y + 268, nt, foc ? C_BG : C_TEXT, 255, 0, 1);
      } else if (jkx_fixed(&ui->cat->jkx[ui->jk_art[i]]) && in && ui->jx_tog[i]) {   /* v1.47: FIXO com ATIVADO/DESATIVADO */
        jx_toggle_pill(ui, x + 16, y + 246, cw - 32, 34, ui->jx_on[i], foc);
      } else if (jkx_fixed(&ui->cat->jkx[ui->jk_art[i]])) {      /* v1.43d: vem com o placar */
        if (!in && jkx_fixed_files(&ui->cat->jkx[ui->jk_art[i]], pl_board_cur(ui))) jx_pill(ui, x + 16, y + 246, cw - 32, 34, "INSTALAR", foc ? 1 : 0, 0);   /* v1.47: baixa so ele */
        else if (in && ui->jx_on[i]) {
          gfx_rrect(s, x + 16, y + 246, cw - 32, 34, 8, foc ? C_FOCUS : C_BASE, 255, 0);
          gfx_rrect(s, x + 16, y + 246, cw - 32, 34, 8, C_BASE_EDGE, 255, 2);
          text_draw_in(s, ui->fb, 15, x + 16, cw - 32, y + 269, "\xE2\x9C\x93 ATIVADO", foc ? C_BG : C_TEXT, 255, 0, 1);
        } else jk_pill(ui, x + 16, y + 246, cw - 32, 34, "VEM COM O JK-MOD", foc ? 0 : -1);
      } else {
        int pw = (cw - 40) / 2;
        if (in && ui->jx_tog[i]) jx_toggle_pill(ui, x + 14, y + 246, pw, 34, ui->jx_on[i], foc && ui->jk_btn == 0);   /* v1.47 */
        else jx_pill(ui, x + 14, y + 246, pw, 34, "INSTALAR", foc && ui->jk_btn == 0 ? 1 : (in ? 2 : 0), 0);
        jx_pill(ui, x + 26 + pw, y + 246, pw, 34, "DESINSTALAR", !in ? -1 : (foc && ui->jk_btn == 1 ? 1 : 0), 1);
      }
      {                                                          /* v1.47: triangulo = video / aviso */
        const CatJkx *cx = &ui->cat->jkx[ui->jk_art[i]];
        const char *tg = cx->video[0] ? "\xE2\x96\xB3 V\xC3\x8D" "DEO" : jkx_aviso(cx) ? "\xE2\x96\xB3 AVISO" : NULL;
        if (tg) {
          int bw = 96, bx = x + cw - bw - 10, by = y + 10;
          gfx_rrect(s, bx, by, bw, 26, 8, cx->video[0] ? RGB(0x0c, 0x1e, 0x34) : RGB(0x3a, 0x2e, 0x08), 235, 0);
          gfx_rrect(s, bx, by, bw, 26, 8, cx->video[0] ? C_CYAN : RGB(0xf5, 0xc5, 0x18), 255, 1);
          text_draw_in(s, ui->fb, 13, bx, bw, by + 18, tg, cx->video[0] ? C_TEXT : RGB(0xff, 0xe0, 0x70), 255, 0, 1);
        }
      }
    } else if (narr) {
      static const char *lab[2] = { "ORIGINAL", "ESTRANGEIRA" };
      for (int k = 0; k < 2; k++) {
        int st = ui->jk_b[i][k] < 0 ? -1 : (foc && ui->jk_btn == k ? 1 : (ui->jk_inst[i] == k + 1 ? 2 : 0));
        jk_pill(ui, x + 18 + k * 126, y + 197, 118, 30, lab[k], st);
      }
    } else if (ui->jk_inst[i]) {
      gfx_rrect(s, x + cw - 92, y + 12, 78, 26, 8, C_BASE, 255, 0);
      text_draw_in(s, ui->fb, 15, x + cw - 92, 78, y + 31, "ATIVO", C_TEXT, 255, 1, 1);
    }
    if (foc) pl_glow(s, x, y, cw, ch, 14, C_FOCUS);
  }
  if (ui->pl_cat == 3) {                                         /* v1.43d: tem mais cards acima/abaixo */
    int rows = (ui->jk_n + JX_COLS - 1) / JX_COLS;
    if (ui->jx_top + JX_ROWS < rows) text_draw_in(s, ui->fb, 18, 0, W, 994, "\xE2\x96\xBC  mais plugins  \xE2\x96\xBC", C_MUTED, 255, 0, 1);
    else if (ui->jx_top > 0) text_draw_in(s, ui->fb, 18, 0, W, 994, "\xE2\x96\xB2  mais plugins acima  \xE2\x96\xB2", C_MUTED, 255, 0, 1);
  }
}

static void pl_open(UI *ui) {
  ui->pl_sel = 0; ui->pl_ed = 0; ui->pl_row = 0;
  if (ui->pl_cat >= jk_ntabs(ui)) ui->pl_cat = 0;                /* v1.43: catalogo sem JKX */
  pl_scan(ui);
  pl_load_images(ui);
  const CatPlBoard *b = pl_board_cur(ui);
  if (b) for (int p = 0; p < b->npack; p++) if (pl_pack_installed(ui, &b->pack[p])) { ui->pl_sel = p; break; }   /* abre no combo que ja esta no console */
  jk_build(ui);                                                 /* v1.40 */
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
/* v1.49/1.50: Mundial, Sula, Supercopa e Recopa nao tem "NAO TROCAR": sem escolha, fica o que ja esta instalado ou o padrao ("MUNDIAL_" / "SULA_Caze" / o 1o) */
static int pl_def_opt(UI *ui, const CatPlBoard *b, int c) {
  if (ui->pl_inst[c] >= 0) return ui->pl_inst[c];
  int first = -1;
  const char *pref = c == 4 ? "MUNDIAL_" : c == 5 ? "SULA_Caze" : "";
  for (int i = 0; i < b->nopt; i++) if (b->opt[i].comp == c) { if (first < 0) first = i; if (pref[0] && !strncmp(b->opt[i].id, pref, strlen(pref)) && (c == 4 || !strcmp(b->opt[i].id, pref))) return i; }
  return first;
}
static void pl_install(UI *ui, const int *opt0, const char *what) {
  const CatPlBoard *b = pl_board_cur(ui);
  if (!b) return;
  int opt[CAT_PL_COMPS];
  for (int c = 0; c < CAT_PL_COMPS; c++) opt[c] = opt0[c];
  for (int c = 4; c < CAT_PL_COMPS; c++) if (opt[c] < 0) opt[c] = pl_def_opt(ui, b, c);
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
  for (int c = 4; c < CAT_PL_COMPS; c++)                         /* v1.43: 5o placar (opcional); v1.45: 6o (Sula) */
    if (opt[c] >= 0 && opt[c] < b->nopt && b->opt[opt[c]].url[0]) L += (size_t)snprintf(cb.extra + L, sizeof(cb.extra) - L, "|%s", b->opt[opt[c]].url);
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
  for (int c = 4; c < CAT_PL_COMPS; c++) ui->pl_pick[c] = ui->pl_inst[c];   /* v1.43: -1 = nao troca */
  for (int c = 4; c < CAT_PL_COMPS; c++) if (ui->pl_pick[c] < 0) ui->pl_pick[c] = pl_def_opt(ui, b, c);   /* v1.50: sem "NAO TROCAR" */
  ui->pl_ed = 1; ui->pl_row = 0; ui->dirty = 1;
}

/* proxima/anterior opcao da competicao c (dir = +1 / -1), com volta */
static void pl_cycle(UI *ui, int c, int dir) {
  const CatPlBoard *b = pl_board_cur(ui);
  if (!b) return;
  int idx[CAT_MAX_PLO + 1], n = 0, at = 0;
  /* v1.50: nenhuma competicao opcional tem "NAO TROCAR" */
  if (0) idx[n++] = -1;                                    /* v1.43: "NAO TROCAR" (Sula, Supercopa, Recopa); v1.49: o Placar Mundial (4) e obrigatorio */
  for (int i = 0; i < b->nopt; i++) if (b->opt[i].comp == c) { if (i == ui->pl_pick[c]) at = n; idx[n++] = i; }
  if (n <= 0) return;
  at = (at + dir + n) % n;
  ui->pl_pick[c] = idx[at];
  ui->dirty = 1;
}

static void pl_key(UI *ui, int key) {
  const CatPlBoard *b = pl_board_cur(ui);
  int nc = pl_ncards(b);
  if (!ui->pl_ed && (key == UI_L2 || key == UI_R2)) {          /* v1.40: troca a aba */
    int nt = jk_ntabs(ui);
    ui->pl_cat = (ui->pl_cat + (key == UI_R2 ? 1 : nt - 1)) % nt;
    ui->jk_sel = 0; ui->jk_btn = 0;
    jk_build(ui);
    return;
  }
  if (ui->jx_modal) {                                            /* v1.44: AVISO aberto: X ou O fecha */
    if (key == UI_OK || key == UI_BACK) { ui->jx_modal = 0; ui->dirty = 1; }
    return;
  }
  if (!ui->pl_ed && ui->pl_cat > 0) { jk_key(ui, key); return; }
  if (ui->pl_ed) {
    switch (key) {
      case UI_UP:    if (ui->pl_row > 0) { ui->pl_row--; ui->dirty = 1; } break;
      case UI_DOWN:  if (ui->pl_row < pl_ncomp(b) - 1) { ui->pl_row++; ui->dirty = 1; } break;
      case UI_LEFT:  pl_cycle(ui, pl_row_comp(b, ui->pl_row), -1); break;
      case UI_RIGHT: pl_cycle(ui, pl_row_comp(b, ui->pl_row), +1); break;
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
  if (ui->pl_cat > 0 && !ui->jk_scanned && !ui->busy) { jk_scan(ui); ui->dirty = 1; }
  jx_card_video_tick(ui);                                        /* v1.47 */
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
  if (!pli_nobox) gfx_rrect(s, x, y, iw, ih, 10, C_DEEP, 255, 0);
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
  int nc = 4, rs = PL_RS;                                        /* v1.43: o card do combo mostra so as 4 (a 5a fica no Monte o seu) */
  for (int c = 0; c < nc; c++) {
    int ry = y + PL_PAD + c * rs;
    int oi = pk->opt[c];
    const CatPlOpt *o = (oi >= 0 && oi < b->nopt) ? &b->opt[oi] : NULL;
    if (o) tot += o->size;
    T(ui, 1, nc == 5 ? 13 : 14, x + PL_PAD + 2, ry + (nc == 5 ? 12 : 13), PL_COMP[c], C_MUTED, 255, 1);
    if (c == 4 && !o) { T(ui, 0, 15, x + PL_PAD + 2, ry + 40, "o combo não troca este placar", C_MUTED, 160, 0); continue; }
    pl_draw_opt(ui, o, oi, x + PL_PAD, ry + (nc == 5 ? 16 : 18), PL_IW, pli_ih, pli_lw, 0);
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
  int ncp = pl_ncomp(b), sp = ncp >= 7 ? 28 : ncp >= 6 ? 38 : ncp == 5 ? 46 : 56, rh = ncp >= 7 ? 24 : ncp >= 6 ? 33 : ncp == 5 ? 40 : 46;   /* v1.43; v1.45: 6; v1.49: 7 e 8 */
  for (int k = 0; k < ncp; k++) {
    int c = pl_row_comp(b, k);
    int ry = y + 106 + k * sp;                                   /* v1.40: card mais baixo */
    gfx_rrect(s, x + PL_PAD, ry, PL_CW - 2 * PL_PAD, rh, 10, C_DEEP, 255, 0);
    gfx_fill(s, x + PL_PAD, ry + 8, 4, rh - 16, C_ACCENT, 255);
    T(ui, 1, ncp >= 7 ? 14 : ncp >= 6 ? 16 : 18, x + PL_PAD + 18, ry + rh / 2 + (ncp >= 7 ? 5 : ncp >= 6 ? 6 : 7), PL_COMP[c], C_TEXT, 255, 1);
    char n[24];
    snprintf(n, sizeof(n), "%d opções", pl_count(b, c));
    int w = TW(ui, 1, ncp >= 7 ? 15 : 17, n, 0);
    T(ui, 1, ncp >= 7 ? 15 : 17, x + PL_CW - PL_PAD - 16 - w, ry + rh / 2 + (ncp >= 7 ? 5 : 7), n, C_ACCENT, 255, 0);
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
  T(ui, 1, 46, x - 2, 92, "JK-MOD'S", C_TEXT, 255, 4);
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
  if (pl_ncomp(b) >= 7) { py = 100; ph = 870; }                  /* v1.50: 7/8 linhas: painel mais alto (placares maiores) */
  gfx_rrect(s, px, py, pw, ph, 24, C_PANEL, 252, 0);
  gfx_rrect(s, px, py, pw, ph, 24, C_FOCUS, 160, 2);
  gfx_fill(s, px + 40, py + 1, 160, 3, C_FOCUS, 255);
  T(ui, 1, 40, px + 40, py + 62, "MONTE O SEU", C_FOCUS, 255, 3);
  T(ui, 0, 20, px + 40, py + 96, "\xE2\x96\xB2\xE2\x96\xBC escolhe a competição   \xE2\x97\x80\xE2\x96\xB6 troca o placar   X baixa o seu combo", C_MUTED, 255, 0);
  { int w = TW(ui, 1, 20, b->title, 2) + 40; gfx_rrect(s, px + pw - 40 - w, py + 34, w, 40, 12, C_ACCENT, 255, 0);
    text_draw_in(s, ui->fb, 20, px + pw - 40 - w, w, py + 61, b->title, C_DARKTXT, 255, 2, 1); }
  unsigned long long tot = 0;
  int nc = pl_ncomp(b), five = nc >= 5, six = nc >= 6, seven = nc >= 7;   /* v1.43; v1.45: 6 linhas; v1.49: 7 e 8 */
  for (int k = 0; k < nc; k++) {
    int c = pl_row_comp(b, k);
    int pitch = seven ? (nc == 7 ? 96 : 84) : six ? 102 : five ? 120 : 148;
    int ry = py + (seven ? 112 : 124) + k * pitch, rx = px + 40, rw = pw - 80, rh = pitch - 8;
    int foc = ui->pl_row == k;
    gfx_rrect(s, rx, ry, rw, rh, 16, foc ? C_PANEL2 : C_DEEP, 255, 0);
    if (foc) pl_glow(s, rx, ry, rw, rh, 16, C_FOCUS); else gfx_rrect(s, rx, ry, rw, rh, 16, C_EDGE, 255, 1);
    int oi = ui->pl_pick[c];
    const CatPlOpt *o = (oi >= 0 && oi < b->nopt) ? &b->opt[oi] : NULL;
    if (o) tot += o->size;
    int yl = ry + (seven ? 24 : six ? 36 : 46), yn = ry + (seven ? rh - 24 : six ? 64 : five ? 72 : 84), y3 = ry + (seven ? rh - 8 : six ? 86 : five ? 98 : 116);
    int fl = seven ? 18 : 21, fn = seven ? 20 : six ? 22 : 24, f3 = seven ? 14 : 18;
    T(ui, 1, fl, rx + 28, yl, PL_COMP[c], C_TEXT, 255, 1);
    if (o) {
      char nm[64]; text_fit(ui->fb, fn, o->name, 300, nm, sizeof(nm));
      T(ui, 1, fn, rx + 28, yn, nm, C_ACCENT, 255, 0);
      int at = 0, n = 0;
      for (int i = 0; i < b->nopt; i++) if (b->opt[i].comp == c) { if (i == oi) at = n; n++; }
      char pos[24]; snprintf(pos, sizeof(pos), "%d de %d", at + 1, n);
      T(ui, 0, f3, rx + 28, y3, pos, C_MUTED, 255, 0);
      if (PL_NOTE[c][0]) { char nt[96]; int pw3 = TW(ui, 0, f3, pos, 0); text_fit(ui->f, f3, PL_NOTE[c], 290 - pw3, nt, sizeof(nt)); T(ui, 0, f3, rx + 28 + pw3 + 14, y3, nt, C_YELLOW, 255, 0); }
    } else if (c >= 5) {
      T(ui, 1, fn, rx + 28, yn, "NÃO TROCAR", C_MUTED, 255, 0);
      { char nt[96]; text_fit(ui->f, f3, PL_NOTE[c], 300, nt, sizeof(nt)); T(ui, 0, f3, rx + 28, y3, nt, C_YELLOW, 255, 0); }
    }
    uint32_t ac = foc ? C_FOCUS : C_MUTED;
    text_draw_in(s, ui->fb, 34, rx + 330, 40, ry + rh / 2 + 12, "\xE2\x97\x80", ac, foc ? 255 : 120, 0, 1);
    pli_nobox = seven;
    pl_draw_opt(ui, o, oi, rx + 380, ry + (seven ? 1 : 12), PL_EIW, pli_eih, pli_elw, 1);
    pli_nobox = 0;
    text_draw_in(s, ui->fb, 34, rx + 380 + PL_EIW + 12, 40, ry + rh / 2 + 12, "\xE2\x96\xB6", ac, foc ? 255 : 120, 0, 1);
  }
  char t[64], mb[24];
  pl_mb(tot, mb, sizeof(mb));
  snprintf(t, sizeof(t), "BAIXAR MEU COMBO  \xC2\xB7  %s", mb);
  int bw = 560;
  pl_button(ui, px + (pw - bw) / 2, py + ph - (seven ? 68 : 78), bw, 56, t, 1, 0);
}

static void pl_draw(UI *ui) {
  Image *s = ui->s;
  if (!ui->pl_bg) pl_build_bg(ui);
  if (ui->pl_bg) gfx_blit_surface(s, ui->pl_bg); else gfx_fill(s, 0, 0, W, H, C_BG, 255);
  const CatPlBoard *b = pl_board_cur(ui);
  pl_draw_header(ui, b);
  jk_tabs(ui);                                                   /* v1.40 */
  if (ui->pl_cat > 0) { jk_draw(ui); pl_draw_msg(ui); jx_draw_modal(ui); return; }
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
  } else if (ui->pl_cat > 0) {                                 /* v1.40 */
    x = hint(ui, x, "\xE2\x97\x80 \xE2\x96\xB2 \xE2\x96\xBC \xE2\x96\xB6", ui->pl_cat == 2 ? "Escolher clube" : "Escolher");
    x = hint(ui, x, "X", ui->pl_cat == 3 ? "Instalar / Ativar" : ui->locked ? "Bloqueado" : "Baixar");
    if (ui->pl_cat == 3 && ui->jk_n > 0) {                       /* v1.47 */
      const CatJkx *fx = &ui->cat->jkx[ui->jk_art[ui->jk_sel]];
      if (fx->video[0]) x = hint(ui, x, "\xE2\x96\xB3", "Vídeo");
      else if (jkx_aviso(fx)) x = hint(ui, x, "\xE2\x96\xB3", "Aviso");
    }
    x = hint(ui, x, "L2  R2", "Categoria");
    if (ui->cat && ui->cat->nplb > 1) x = hint(ui, x, "L1  R1", "Patch");
    x = hint(ui, x, "O", "Voltar");
  } else {
    const CatPlBoard *b = pl_board_cur(ui);
    x = hint(ui, x, "\xE2\x97\x80 \xE2\x96\xB2 \xE2\x96\xBC \xE2\x96\xB6", "Escolher combo");
    x = hint(ui, x, "X", ui->locked ? "Bloqueado" : ((b && b->monte && ui->pl_sel == pl_ncards(b) - 1) ? "Montar" : "Baixar"));
    x = hint(ui, x, "L2  R2", "Categoria");
    if (ui->cat && ui->cat->nplb > 1) x = hint(ui, x, "L1  R1", "Patch");
    x = hint(ui, x, "O", "Voltar");
  }
  if (update_available(ui)) x = hint(ui, x, "\xE2\x96\xA1", "Atualizar app");
  *px = x;
}
