#include "ui.h"
#include "version.h"
#include "vplayer.h"

#include <stdio.h>
#include <pthread.h>
#include <sys/stat.h>
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
static int g_bh = BTN_H;                /* altura do botao em uso (menor quando o painel tem 4 itens) */
#define TOAST_TICKS  360
#define SPLASH_FADE 140            /* ~2,3 s: o conteudo comeca a sumir */
#define SPLASH_END  158            /* ~2,6 s no total (60 ticks por segundo) */
#define SPLASH_SKIP_FROM 30        /* depois de 0,5 s qualquer tecla pula */

enum { MODE_LOADING = 0, MODE_LOGIN, MODE_CATALOG, MODE_QR };
#define FLD_MAX 64

struct UI {
  int mode;
  UiLoginFn login_fn;
  UiLoginFn register_fn;
  Image *qr;                           /* QR Code do WhatsApp do admin (assets/images/qr_whatsapp.png) */
  char qr_user[64];
  UiLogoutFn logout_fn;
  UiOpenUrlFn open_fn;
  void *open_ud;
  UiCoverFetchFn cover_fetch;          /* baixa capas remotas (https) */
  void *cover_ud;
  char cover_dir[200];
  int cover_gen;                       /* muda a cada catalogo novo: downloads antigos sao ignorados */
  volatile int cover_pending;          /* ha capa baixada esperando ser carregada */
  int cover_ready[CAT_MAX_GAMES];
  void *auth_ud;
  char field[2][FLD_MAX + 1];          /* 0 = usuario, 1 = senha */
  int cur_field, kr, kc, caps, show_pass;
  char login_msg[200];
  char loading_msg[120];
  char username[64];
  Image *s;
  Image *logo;                         /* selo JK Patchs do cabecalho (assets/images/logo_jk.png) */
  Image *logo_full;                    /* original (350x350) para a abertura e a tela de login */
  int splash;                          /* ticks da animacao de abertura; >= SPLASH_END = terminou */
  Image *splash_base;                  /* fundo da abertura, pronto (so copia por quadro) */
  Image *splash_logo;                  /* logo ja no tamanho final (sem redimensionar por quadro) */
  const Catalog *cat;
  UiInstallFn fn;
  void *user;
  Font *f, *fb;
  Image *bg;
  Image *thumb[CAT_MAX_GAMES];

  int filt[CAT_MAX_GAMES], nfilt;
  int cat_idx, sel, scroll, focus;

  int busy, dirty;
  int updating;                        /* o pedido em andamento e a atualizacao do proprio app */
  unsigned frame;
  char toast[256];
  int toast_left, toast_kind;
  char assets[256];

  /* video dentro do app: 0 = nenhum, 1 = janela, 2 = minimizado (PiP) */
  int vid_mode, vdirty, base_stale, vlast_state, vlast_pct, vlast_sec;
  char vtitle[96], vurl[240];
  Image *vframe, *base;
};

/* ------------------------------------------------------------------ utilidades */
static void refilter(UI *ui) {
  ui->nfilt = 0;
  if (!ui->cat) return;
  const char *id = ui->cat->cat[ui->cat_idx].id;
  for (int i = 0; i < ui->cat->ngame; i++) {
    if (ui->cat_idx == 0 || !strcmp(ui->cat->game[i].category, id)) ui->filt[ui->nfilt++] = i;
  }
}

static const CatGame *cur_game(UI *ui) {
  if (!ui->cat || ui->nfilt <= 0) return NULL;
  return &ui->cat->game[ui->filt[ui->sel]];
}

/* Botao que ja vem focado: o ultimo que nao e "base" (normalmente o update); senao o primeiro. */
static int default_focus(const CatGame *g) {
  if (!g) return 0;
  for (int i = g->nbtn - 1; i >= 0; i--) if (!g->btn[i].base) return i;
  return 0;
}

/* Itens focaveis do painel: os botoes de download e, por ultimo, o botao de video (se houver). */
static int panel_items(const CatGame *g) { return g ? g->nbtn + (g->video[0] ? 1 : 0) : 0; }

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

/* ---- capas remotas: o app guarda a imagem baixada em cover_dir (nome = hash do link) e a reaproveita nas proximas aberturas.
 * Para trocar uma capa: suba a imagem NOVA no R2 com outro nome (ou ...png?v=2) e troque o link no catalogo. */
static pthread_mutex_t cover_mu = PTHREAD_MUTEX_INITIALIZER;
typedef struct { UI *ui; int gen, n; int idx[CAT_MAX_GAMES]; char url[CAT_MAX_GAMES][256]; char dest[CAT_MAX_GAMES][320]; } CoverJobs;

static int is_remote_cover(const char *c) { return !strncmp(c, "https://", 8); }

static void cover_cache_path(const UI *ui, const char *url, char *out, size_t cap) {
  unsigned h = 2166136261u;
  for (const char *p = url; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  snprintf(out, cap, "%s/%08x.img", ui->cover_dir, h);
}

static void *cover_worker(void *arg) {
  CoverJobs *J = (CoverJobs *)arg;
  UI *ui = J->ui;
  for (int k = 0; k < J->n; k++) {
    pthread_mutex_lock(&cover_mu); int cur = ui->cover_gen; pthread_mutex_unlock(&cover_mu);
    if (cur != J->gen) break;                                   /* catalogo mudou: este lote nao serve mais */
    char tmp[340];
    snprintf(tmp, sizeof(tmp), "%s.tmp", J->dest[k]);
    if (ui->cover_fetch(J->url[k], tmp, ui->cover_ud) == 0 && rename(tmp, J->dest[k]) == 0) {
      pthread_mutex_lock(&cover_mu);
      if (ui->cover_gen == J->gen) { ui->cover_ready[J->idx[k]] = 1; ui->cover_pending = 1; }
      pthread_mutex_unlock(&cover_mu);
    } else {
      remove(tmp);
    }
  }
  free(J);
  return NULL;
}

static void cover_start(UI *ui, CoverJobs *J) {
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int r = pthread_create(&t, &at, cover_worker, J);
  pthread_attr_destroy(&at);
  if (r != 0) free(J);
  (void)ui;
}

static Image *thumb_from_file(const char *path) {
  Image *raw = img_load(path);
  if (!raw) return NULL;
  Image *t = img_scale_cover(raw, COVER_W, COVER_H);
  img_free(raw);
  return t;
}

static void load_thumbs(UI *ui) {
  char path[320];
  pthread_mutex_lock(&cover_mu);
  ui->cover_gen++;
  for (int i = 0; i < CAT_MAX_GAMES; i++) ui->cover_ready[i] = 0;
  ui->cover_pending = 0;
  int gen = ui->cover_gen;
  pthread_mutex_unlock(&cover_mu);
  for (int i = 0; i < CAT_MAX_GAMES; i++) { if (ui->thumb[i]) { img_free(ui->thumb[i]); ui->thumb[i] = NULL; } }
  if (!ui->cat) return;
  CoverJobs *J = NULL;
  for (int i = 0; i < ui->cat->ngame; i++) {
    const char *cv = ui->cat->game[i].cover;
    if (!cv[0]) continue;
    if (is_remote_cover(cv)) {
      if (!ui->cover_fetch || !ui->cover_dir[0]) continue;
      cover_cache_path(ui, cv, path, sizeof(path));
      ui->thumb[i] = thumb_from_file(path);                     /* ja baixada antes? */
      if (!ui->thumb[i]) {
        const char *base = strrchr(cv, '/'); base = base ? base + 1 : cv;       /* capa de reserva dentro do app, se tiver */
        char nm[96]; snprintf(nm, sizeof(nm), "%s", base);
        char *q = strchr(nm, '?'); if (q) *q = 0;
        char bp[512]; snprintf(bp, sizeof(bp), "%s/images/%s", ui->assets, nm);
        ui->thumb[i] = thumb_from_file(bp);
        if (!J) { J = (CoverJobs *)calloc(1, sizeof(CoverJobs)); if (J) { J->ui = ui; J->gen = gen; } }
        if (J && J->n < CAT_MAX_GAMES) {
          snprintf(J->url[J->n], sizeof(J->url[0]), "%s", cv);
          snprintf(J->dest[J->n], sizeof(J->dest[0]), "%s", path);
          J->idx[J->n++] = i;
        }
      }
      continue;
    }
    snprintf(path, sizeof(path), "%s/images/%s", ui->assets, cv);
    ui->thumb[i] = thumb_from_file(path);
  }
  if (J) { if (J->n > 0) cover_start(ui, J); else free(J); }
}

/* chamado do ui_tick: carrega as capas que terminaram de baixar */
static void cover_poll(UI *ui) {
  if (!ui->cover_pending) return;
  int ready[CAT_MAX_GAMES];
  pthread_mutex_lock(&cover_mu);
  memcpy(ready, ui->cover_ready, sizeof(ready));
  memset(ui->cover_ready, 0, sizeof(ui->cover_ready));
  ui->cover_pending = 0;
  pthread_mutex_unlock(&cover_mu);
  for (int i = 0; i < CAT_MAX_GAMES && ui->cat; i++) {
    if (!ready[i] || i >= ui->cat->ngame) continue;
    char path[320];
    cover_cache_path(ui, ui->cat->game[i].cover, path, sizeof(path));
    Image *t = thumb_from_file(path);
    if (t) { if (ui->thumb[i]) img_free(ui->thumb[i]); ui->thumb[i] = t; ui->dirty = 1; }
    else remove(path);                                          /* arquivo ruim: apaga para tentar de novo na proxima abertura */
  }
}

void ui_set_cover_fetcher(UI *ui, UiCoverFetchFn fn, const char *dir, void *ud) {
  ui->cover_fetch = fn; ui->cover_ud = ud;
  snprintf(ui->cover_dir, sizeof(ui->cover_dir), "%s", dir ? dir : "");
  if (ui->cover_dir[0]) mkdir(ui->cover_dir, 0777);
}

UI *ui_create(Image *surface, const Catalog *cat, const char *assets_dir, UiInstallFn fn, void *user) {
  UI *ui = (UI *)calloc(1, sizeof(UI));
  if (!ui) return NULL;
  ui->s = surface;
  ui->cat = cat;
  ui->fn = fn;
  ui->user = user;
  snprintf(ui->assets, sizeof(ui->assets), "%s", assets_dir);
  {
    char lp[300];
    snprintf(lp, sizeof(lp), "%s/images/logo_jk.png", assets_dir);
    Image *raw = img_load(lp);
    if (raw) { ui->logo = img_scale(raw, 88, 88); ui->logo_full = raw; }
    snprintf(lp, sizeof(lp), "%s/images/qr_whatsapp.png", assets_dir);
    ui->qr = img_load(lp);
  }

  char path[320];
  snprintf(path, sizeof(path), "%s/fonts/DejaVuSans.ttf", assets_dir);
  ui->f = font_load(path);
  snprintf(path, sizeof(path), "%s/fonts/DejaVuSans-Bold.ttf", assets_dir);
  ui->fb = font_load(path);
  if (!ui->f) ui->f = ui->fb;
  if (!ui->fb) ui->fb = ui->f;

  ui->bg = build_bg(ui);

  if (cat) { load_thumbs(ui); ui->mode = MODE_CATALOG; }
  else { ui->mode = MODE_LOADING; snprintf(ui->loading_msg, sizeof(ui->loading_msg), "Iniciando..."); }

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

void ui_set_auth_handlers(UI *ui, UiLoginFn login, UiLogoutFn logout, void *ud) {
  ui->login_fn = login; ui->logout_fn = logout; ui->auth_ud = ud;
}

void ui_set_open_handler(UI *ui, UiOpenUrlFn fn, void *ud) { ui->open_fn = fn; ui->open_ud = ud; }
void ui_set_register_handler(UI *ui, UiLoginFn fn) { ui->register_fn = fn; }

void ui_show_register_done(UI *ui, const char *user) {
  ui->mode = MODE_QR;
  snprintf(ui->qr_user, sizeof(ui->qr_user), "%s", user ? user : "");
  memset(ui->field[1], 0, sizeof(ui->field[1]));
  ui->busy = 0;
  ui->dirty = 1;
}

void ui_show_loading(UI *ui, const char *msg) {
  ui->mode = MODE_LOADING;
  snprintf(ui->loading_msg, sizeof(ui->loading_msg), "%s", msg ? msg : "Carregando...");
  ui->dirty = 1;
}

void ui_show_login(UI *ui, const char *msg) {
  ui->mode = MODE_LOGIN;
  snprintf(ui->login_msg, sizeof(ui->login_msg), "%s", msg ? msg : "");
  memset(ui->field[1], 0, sizeof(ui->field[1]));                 /* a senha nunca fica guardada */
  ui->show_pass = 0;
  ui->cur_field = ui->field[0][0] ? 1 : 0;
  ui->kr = 1; ui->kc = 0;
  ui->busy = 0;
  ui->dirty = 1;
}

void ui_set_user(UI *ui, const char *name) {
  snprintf(ui->username, sizeof(ui->username), "%s", name ? name : "");
  ui->dirty = 1;
}

void ui_set_catalog(UI *ui, const Catalog *cat) {
  ui->cat = cat;
  load_thumbs(ui);
  ui->cat_idx = 0;
  ui->scroll = 0;
  refilter(ui);
  select_game(ui, 0);
  ui->mode = MODE_CATALOG;
  ui->busy = 0;
  ui->dirty = 1;
}

/* ---- teclado na tela */
static const char *KB_N[4] = {"1234567890", "qwertyuiop", "asdfghjkl_", "zxcvbnm.-@"};
static const char *KB_S[4] = {"!#$%^&*()=", "QWERTYUIOP", "ASDFGHJKL:", "ZXCVBNM,+/"};
#define KB_COLS 10
#define KB_ACTIONS 6          /* MAIUSC, MOSTRAR, APAGAR, CAMPO, CADASTRAR, ENTRAR */
enum { ACT_CAPS = 0, ACT_SHOW, ACT_DEL, ACT_FIELD, ACT_REG, ACT_GO };

static void login_msg(UI *ui, const char *m) {
  snprintf(ui->login_msg, sizeof(ui->login_msg), "%s", m);
  ui->dirty = 1;
}

static void field_add(UI *ui, char c) {
  char *f = ui->field[ui->cur_field];
  size_t n = strlen(f);
  if (n >= FLD_MAX) return;
  if (ui->cur_field == 0 && n >= 32) return;                    /* o servidor limita o usuario a 32 */
  f[n] = c; f[n + 1] = 0;
  ui->login_msg[0] = 0;
  ui->dirty = 1;
}

static void field_del(UI *ui) {
  char *f = ui->field[ui->cur_field];
  size_t n = strlen(f);
  if (n) { f[n - 1] = 0; ui->dirty = 1; }
}

static void login_submit(UI *ui) {
  if (!ui->field[0][0]) { ui->cur_field = 0; login_msg(ui, "Digite o usuário."); return; }
  if (!ui->field[1][0]) { ui->cur_field = 1; login_msg(ui, "Digite a senha."); return; }
  if (!ui->login_fn) return;
  char u[FLD_MAX + 1], p[FLD_MAX + 1];
  snprintf(u, sizeof(u), "%s", ui->field[0]);
  snprintf(p, sizeof(p), "%s", ui->field[1]);
  memset(ui->field[1], 0, sizeof(ui->field[1]));
  ui_show_loading(ui, "Entrando...");
  ui->login_fn(u, p, ui->auth_ud);
  memset(p, 0, sizeof(p));
}

/* Cadastro: valida aqui (o servidor valida de novo) e envia; a conta nasce pendente. */
static void register_submit(UI *ui) {
  const char *u = ui->field[0], *p = ui->field[1];
  size_t un = strlen(u), pn = strlen(p);
  if (un < 2) { ui->cur_field = 0; login_msg(ui, "Escolha um usuário com pelo menos 2 caracteres."); return; }
  for (size_t i = 0; i < un; i++) {
    char c = u[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) {
      ui->cur_field = 0; login_msg(ui, "Usuário: só letras minúsculas, números, . _ -"); return;
    }
  }
  if (pn < 8) { ui->cur_field = 1; login_msg(ui, "A senha precisa ter pelo menos 8 caracteres."); return; }
  if (!ui->register_fn) return;
  char uu[FLD_MAX + 1], pp[FLD_MAX + 1];
  snprintf(uu, sizeof(uu), "%s", u);
  snprintf(pp, sizeof(pp), "%s", p);
  memset(ui->field[1], 0, sizeof(ui->field[1]));
  ui_show_loading(ui, "Enviando cadastro...");
  ui->register_fn(uu, pp, ui->auth_ud);
  memset(pp, 0, sizeof(pp));
}

static void login_key(UI *ui, int key) {
  switch (key) {
    case UI_LEFT:  if (ui->kc > 0) { ui->kc--; ui->dirty = 1; } break;
    case UI_RIGHT: {
      int max = ui->kr == 4 ? KB_ACTIONS : KB_COLS;
      if (ui->kc + 1 < max) { ui->kc++; ui->dirty = 1; }
      break;
    }
    case UI_UP:
      if (ui->kr > 0) {
        if (ui->kr == 4) { ui->kc = ui->kc * 2; if (ui->kc > KB_COLS - 1) ui->kc = KB_COLS - 1; }
        ui->kr--; ui->dirty = 1;
      }
      break;
    case UI_DOWN:
      if (ui->kr < 4) {
        if (ui->kr == 3) ui->kc = ui->kc / 2;
        ui->kr++; ui->dirty = 1;
      }
      break;
    case UI_OK:
      if (ui->kr < 4) {
        field_add(ui, (ui->caps ? KB_S : KB_N)[ui->kr][ui->kc]);
      } else if (ui->kc == ACT_CAPS)  { ui->caps = !ui->caps; ui->dirty = 1; }
      else if (ui->kc == ACT_SHOW)    { ui->show_pass = !ui->show_pass; ui->dirty = 1; }
      else if (ui->kc == ACT_DEL)     field_del(ui);
      else if (ui->kc == ACT_FIELD)   { ui->cur_field = !ui->cur_field; ui->dirty = 1; }
      else if (ui->kc == ACT_REG)     register_submit(ui);
      else                            login_submit(ui);
      break;
    case UI_BACK:
    case UI_SQUARE:   field_del(ui); break;
    case UI_TRIANGLE:
    case UI_L1:
    case UI_R1:       ui->cur_field = !ui->cur_field; ui->dirty = 1; break;
    case UI_OPTIONS:  login_submit(ui); break;
    default: break;
  }
}

/* 1 se o servidor anunciou uma versao do app mais nova que esta (e ha link para baixar). */
static int update_available(const UI *ui) {
  if (!ui->cat || !ui->cat->app.url[0] || !ui->cat->app.content_id[0]) return 0;
  return catalog_version_num(ui->cat->app.version) > catalog_version_num(APP_VERSION);
}

static void trigger_update(UI *ui) {
  if (!update_available(ui)) { ui_toast(ui, "Seu app já está na versão mais recente.", UI_TOAST_INFO); return; }
  if (ui->busy) { ui_toast(ui, "Já existe um pedido em andamento. Aguarde.", UI_TOAST_INFO); return; }
  const CatApp *a = &ui->cat->app;
  static CatButton b;                                   /* a copia fica viva ate o app terminar de usar */
  memset(&b, 0, sizeof(b));
  snprintf(b.label, sizeof(b.label), "JK Patchs %s", a->version);
  snprintf(b.name, sizeof(b.name), "JK Patchs %s", a->version);
  snprintf(b.url, sizeof(b.url), "%s", a->url);
  snprintf(b.content_id, sizeof(b.content_id), "%s", a->content_id);
  snprintf(b.type, sizeof(b.type), "%s", a->type[0] ? a->type : "PS4GD");
  b.size = a->size;
  ui_toast(ui, "Enviando a atualização do app...", UI_TOAST_INFO);
  ui->busy = 1;
  ui->updating = 1;
  if (ui->fn) ui->fn(&b, ui->user);
}

void ui_install_done(UI *ui, int code, const char *err) {
  ui_set_busy(ui, 0);
  if (code == 0 && ui->updating)
    ui_toast(ui, "Atualização enviada! Quando terminar nas Notificações, feche e abra o app.", UI_TOAST_OK);
  else if (code == 0)
    ui_toast(ui, "Pedido aceito! Acompanhe nas Notificações do PS4.", UI_TOAST_OK);
  else
    ui_toast(ui, (err && err[0]) ? err : "Falha ao enviar o pedido.", UI_TOAST_ERR);
  ui->updating = 0;
}

static int is_jkv(const char *u);
static void vid_open(UI *ui, const CatGame *g);

static void trigger_install(UI *ui) {
  const CatGame *g = cur_game(ui);
  if (!g || panel_items(g) == 0) return;
  if (ui->focus >= g->nbtn) {                                   /* botao de video */
    if (g->video[0] && is_jkv(g->video)) { vid_open(ui, g); return; }          /* player dentro do app */
    if (g->video[0] && ui->open_fn) {
      ui_toast(ui, "Abrindo o vídeo no navegador do PS4...", UI_TOAST_INFO);
      ui->open_fn(g->video, ui->open_ud);
    }
    return;
  }
  const CatButton *b = &g->btn[ui->focus];
  if (ui->busy) { ui_toast(ui, "Já existe um pedido em andamento. Aguarde.", UI_TOAST_INFO); return; }
  if (!b->url[0]) { ui_toast(ui, "URL do pacote não configurada neste botão.", UI_TOAST_ERR); return; }
  if (!b->content_id[0]) {
    ui_toast(ui, "Este botão está sem ID do PKG (data-content-id).", UI_TOAST_ERR);
    return;
  }
  ui_toast(ui, "Enviando pedido ao instalador nativo...", UI_TOAST_INFO);
  ui->busy = 1;
  ui->updating = 0;
  if (ui->fn) ui->fn(b, ui->user);
}


/* ------------------------------------------------------------------ video dentro do app */
static void vid_close(UI *ui) {
  if (!ui->vid_mode) return;
  ui->vid_mode = 0;
  vp_stop();
  if (ui->vframe) { img_free(ui->vframe); ui->vframe = NULL; }
  ui->dirty = 1;
}

static int is_jkv(const char *u) {
  size_t n = strlen(u);
  return n > 4 && strcmp(u + n - 4, ".jkv") == 0;
}

static void vid_open(UI *ui, const CatGame *g) {
  if (vp_busy()) { ui_toast(ui, "Aguarde o vídeo anterior terminar de fechar e tente de novo.", UI_TOAST_INFO); return; }
  snprintf(ui->vtitle, sizeof(ui->vtitle), "%s", g->title);
  snprintf(ui->vurl, sizeof(ui->vurl), "%s", g->video);
  if (vp_load(g->video) != 0) { ui_toast(ui, "Não foi possível iniciar o vídeo.", UI_TOAST_ERR); return; }
  ui->vid_mode = 1;
  ui->vlast_state = -1;
  ui->vlast_pct = -1;
  ui->vlast_sec = -1;
  ui->base_stale = 1;
  ui->dirty = 1;
}

static void vid_key(UI *ui, int key) {
  int st = vp_state();
  if (key == UI_BACK && ui->vid_mode == 1) { vid_close(ui); return; }
  if (key == UI_TRIANGLE) {
    if (ui->vid_mode == 1 && st != VP_ERROR) { ui->vid_mode = 2; ui->base_stale = 1; ui->dirty = 1; }
    else vid_close(ui);
    return;
  }
  if (key == UI_OPTIONS && ui->vid_mode == 2) { ui->vid_mode = 1; ui->base_stale = 1; ui->dirty = 1; return; }
  if (key == UI_OK && ui->vid_mode == 1) {
    if (st == VP_PLAYING) { vp_set_paused(!vp_paused()); ui->vdirty = 1; }
    else if (st == VP_ENDED) { if (vp_busy() == 0) vp_load(ui->vurl); }
    else if (st == VP_ERROR) vid_close(ui);
  }
}

static void vid_tick(UI *ui) {
  Image *f = vp_take_frame();
  if (f) { if (ui->vframe) img_free(ui->vframe); ui->vframe = f; ui->vdirty = 1; }
  int st = vp_state();
  if (st == VP_IDLE && !vp_busy()) { vid_close(ui); return; }
  int pct = vp_progress(), sec = (int)vp_position();
  if (st != ui->vlast_state || (st == VP_DOWNLOADING && pct != ui->vlast_pct) || (st == VP_PLAYING && sec != ui->vlast_sec)) {
    ui->vlast_state = st; ui->vlast_pct = pct; ui->vlast_sec = sec;
    ui->vdirty = 1;
  }
}

void ui_key(UI *ui, int key) {
  if (ui->splash < SPLASH_END) {                                  /* durante a abertura as teclas so pulam */
    if (ui->splash >= SPLASH_SKIP_FROM && ui->splash < SPLASH_FADE) ui->splash = SPLASH_FADE;
    return;
  }
  if (ui->mode == MODE_LOGIN) { login_key(ui, key); return; }
  if (ui->mode == MODE_QR) {                                      /* tela do QR: qualquer botao de voltar/confirmar */
    if (key == UI_BACK || key == UI_OK || key == UI_OPTIONS) ui_show_login(ui, "Depois que o admin ativar, toque em ENTRAR.");
    return;
  }
  if (ui->mode != MODE_CATALOG) return;
  if (ui->vid_mode == 1 || (ui->vid_mode == 2 && (key == UI_TRIANGLE || key == UI_OPTIONS))) { vid_key(ui, key); return; }
  if (key == UI_TRIANGLE) {                                     /* sair da conta */
    if (ui->busy) { ui_toast(ui, "Aguarde o pedido atual terminar.", UI_TOAST_INFO); return; }
    vid_close(ui);
    if (ui->logout_fn) ui->logout_fn(ui->auth_ud);
    ui->field[0][0] = 0;
    ui_show_login(ui, "Você saiu da conta.");
    return;
  }
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
      if (g && ui->focus + 1 < panel_items(g)) { ui->focus++; ui->dirty = 1; }
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
    case UI_SQUARE:
      trigger_update(ui);
      break;
    default:
      break;
  }
}

void ui_tick(UI *ui) {
  ui->frame++;
  if (ui->splash < SPLASH_END) {
    ui->splash++;
    if (ui->splash % 2 == 0 || ui->splash >= SPLASH_END) ui->dirty = 1;      /* 30 quadros por segundo bastam e pesam metade */
    if (ui->splash >= SPLASH_END) {                                           /* acabou: libera a memoria da abertura */
      if (ui->splash_base) { img_free(ui->splash_base); ui->splash_base = NULL; }
      if (ui->splash_logo) { img_free(ui->splash_logo); ui->splash_logo = NULL; }
    }
  }
  cover_poll(ui);
  if (ui->vid_mode) vid_tick(ui);
  if ((ui->busy || ui->mode == MODE_LOADING) && ui->frame % 6 == 0) ui->dirty = 1;   /* spinner */
  if (ui->mode == MODE_LOGIN && ui->frame % 30 == 0) ui->dirty = 1;                  /* cursor piscando */
  if (ui->toast_left > 0 && !ui->busy) {
    if (--ui->toast_left == 0) ui->dirty = 1;
  }
}

int ui_take_dirty(UI *ui) {
  int d = ui->dirty || ui->vdirty;
  if (ui->dirty) ui->base_stale = 1;
  ui->dirty = 0;
  ui->vdirty = 0;
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

  int x;
  if (ui->logo) {
    gfx_blit(s, ui->logo, MARGIN, 11, 0, 255);
    x = MARGIN + 88 + 20;
  } else {
    gfx_rrect(s, MARGIN, 28, 84, 54, 12, C_ACCENT, 255, 3);
    text_draw_in(s, ui->fb, 28, MARGIN, 84, 64, "JK", C_ACCENT, 255, 0, 1);
    x = MARGIN + 84 + 22;
  }
  T(ui, 1, 28, x, 66, ui->logo ? "JK PATCHS" : "PATCHS", C_TEXT, 255, 3);

  char st[96];
  if (ui->mode == MODE_CATALOG) {
    snprintf(st, sizeof(st), "ONLINE  \xC2\xB7  ");
    size_t n = strlen(st);
    for (const char *q = ui->username; *q && n + 2 < sizeof(st); q++) st[n++] = (*q >= 'a' && *q <= 'z') ? *q - 32 : *q;
    st[n] = 0;
  } else {
    snprintf(st, sizeof(st), "%s", ui->mode == MODE_LOGIN ? "ACESSO RESTRITO" : ui->mode == MODE_QR ? "CADASTRO ENVIADO" : "CONECTANDO");
  }
  int w = TW(ui, 1, 20, st, 3);
  T(ui, 1, 20, W - MARGIN - w, 62, st, C_MUTED, 255, 3);
  gfx_circle(s, W - MARGIN - w - 22, 55, 7, ui->mode == MODE_CATALOG ? C_ACCENT : C_FOCUS, 255);
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
    gfx_rrect(s, x, y, w, g_bh, r, C_BASE, 255, 0);
    gfx_rrect(s, x, y, w, g_bh, r, C_BASE_EDGE, 255, 2);
  } else {
    gfx_rrect(s, x, y, w, g_bh, r, C_ACCENT, 255, 0);
  }
  uint32_t fg = b->base ? C_WHITE : C_DARKTXT;
  char info[48];
  info_text(b, info, sizeof(info));
  int iw = info[0] ? TW(ui, 0, 20, info, 0) : 0;
  int arrow_w = 56;
  int avail = w - 40 - arrow_w - (iw ? iw + 24 : 0);
  char lab[128];
  text_fit(ui->fb, 27, b->label, avail, lab, sizeof(lab));
  T(ui, 1, 27, x + 32, y + g_bh / 2 + 10, lab, fg, 255, 0);
  if (info[0]) T(ui, 0, 20, x + w - arrow_w - 12 - iw, y + g_bh / 2 + 7, info, fg, 190, 0);
  text_draw_in(s, ui->fb, 30, x + w - arrow_w - 4, arrow_w, y + g_bh / 2 + 11, "\xE2\x86\x93", fg, 255, 0, 1);
  if (!focused) gfx_rrect(s, x, y, w, g_bh, r, C_BG, 70, 0);
  else gfx_rrect(s, x - 8, y - 8, w + 16, g_bh + 16, r + 8, C_FOCUS, 255, 4);
}

static void draw_video_button(UI *ui, int x, int y, int w, int focused) {
  Image *s = ui->s;
  int r = 20;
  gfx_rrect(s, x, y, w, g_bh, r, C_PANEL2, 255, 0);
  gfx_rrect(s, x, y, w, g_bh, r, C_WHITE, 40, 2);
  T(ui, 1, 27, x + 32, y + g_bh / 2 + 10, "VER VÍDEO DO PATCH", C_TEXT, 255, 0);
  T(ui, 0, 20, x + w - 56 - 12 - TW(ui, 0, 20, "Assistir", 0), y + g_bh / 2 + 7, "Assistir", C_MUTED, 255, 0);
  text_draw_in(s, ui->fb, 30, x + w - 60, 56, y + g_bh / 2 + 11, "\xE2\x96\xB6", C_ACCENT, 255, 0, 1);
  if (focused) gfx_rrect(s, x - 8, y - 8, w + 16, g_bh + 16, r + 8, C_FOCUS, 255, 4);
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

  int items = panel_items(g);
  /* ate 4 itens (ex.: FC 27 = 3 downloads + video) cabem todos, com botoes um pouco mais baixos */
  int vis = items <= 4 ? 4 : BTN_VISIBLE;
  int gap = items <= 4 ? 12 : BTN_GAP;
  g_bh = items <= 4 ? 62 : BTN_H;
  int nb = items < vis ? items : vis;
  int bottom = PANEL_Y + PANEL_H - 44;
  int btn_top = bottom - (nb > 0 ? nb * g_bh + (nb - 1) * gap : 0);
  int lines = (btn_top - 28 - desc_y) / 36 + 1;
  if (lines < 1) lines = 1;
  if (lines > 5) lines = 5;
  text_wrap(s, ui->f, 25, px, desc_y, iw, 36, lines, g->desc, C_MUTED, 255);

  if (items == 0) {
    text_draw_in(s, ui->f, 24, px, iw, bottom - 20, "Nenhum download disponível ainda.", C_MUTED, 255, 0, 0);
    return;
  }
  /* janela de botoes que acompanha o foco */
  int first = 0;
  if (ui->focus >= vis) first = ui->focus - vis + 1;
  for (int i = 0; i < nb; i++) {
    int bi = first + i;
    int y = btn_top + i * (g_bh + gap);
    if (bi < g->nbtn) draw_button(ui, &g->btn[bi], px, y, iw, bi == ui->focus);
    else draw_video_button(ui, px, y, iw, bi == ui->focus);
  }
  if (first > 0) T(ui, 1, 22, PANEL_X + PANEL_W - 44, btn_top - 10, "\xE2\x96\xB2", C_MUTED, 255, 0);
  if (first + nb < items) T(ui, 1, 22, PANEL_X + PANEL_W - 44, bottom + 32, "\xE2\x96\xBC", C_MUTED, 255, 0);
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
  if (update_available(ui)) {
    char buf[200];
    snprintf(buf, sizeof(buf), "Atualização disponível: JK Patchs %s (você tem %s).  Aperte  \xE2\x96\xA1  para atualizar.",
             ui->cat->app.version, APP_VERSION);
    int tw = TW(ui, 1, 26, buf, 0);
    int w = tw + 68, h = 62, x = (W - w) / 2, y = 912;
    gfx_rrect(s, x, y, w, h, h / 2, C_PANEL2, 250, 0);
    gfx_rrect(s, x, y, w, h, h / 2, C_FOCUS, 255, 3);
    T(ui, 1, 26, x + 34, y + h / 2 + 9, buf, C_TEXT, 255, 0);
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
  if (ui->mode == MODE_LOGIN) {
    x = hint(ui, x, "\xE2\x97\x80 \xE2\x96\xB2 \xE2\x96\xBC \xE2\x96\xB6", "Mover");
    x = hint(ui, x, "X", "Digitar");
    x = hint(ui, x, "\xE2\x96\xA1", "Apagar");
    x = hint(ui, x, "\xE2\x96\xB3", "Trocar campo");
    x = hint(ui, x, "OPTIONS", "Entrar");
  } else if (ui->mode == MODE_QR) {
    x = hint(ui, x, "O", "Voltar ao login");
  } else if (ui->mode == MODE_CATALOG) {
    x = hint(ui, x, "\xE2\x97\x80 \xE2\x96\xB6", "Coleções");
    x = hint(ui, x, "\xE2\x96\xB2 \xE2\x96\xBC", "Downloads");
    x = hint(ui, x, "X", "Instalar");
    x = hint(ui, x, "L1  R1", "Categorias");
    x = hint(ui, x, "O", "Início");
    x = hint(ui, x, "\xE2\x96\xB3", "Sair");
    if (update_available(ui)) x = hint(ui, x, "\xE2\x96\xA1", "Atualizar app");
  }
  char v[48];
  snprintf(v, sizeof(v), "JK Patchs  v%s", APP_VERSION);
  int w = TW(ui, 0, 20, v, 1);
  T(ui, 0, 20, W - MARGIN - w, 1036, v, C_MUTED, 150, 1);
}

/* ---- telas de login / carregando */
#define LG_X   480
#define LG_W   960
#define LG_Y   130
#define LG_H   810
#define KEY_W  78
#define KEY_H  66
#define KEY_G  10

static void draw_field(UI *ui, int idx, int y) {
  Image *s = ui->s;
  int x = LG_X + 45, w = LG_W - 90, h = 64;
  int active = (ui->cur_field == idx);
  gfx_rrect(s, x, y, w, h, 16, C_BG, 235, 0);
  gfx_rrect(s, x, y, w, h, 16, active ? C_FOCUS : C_WHITE, active ? 255 : 40, active ? 3 : 2);

  const char *ph = idx == 0 ? "Usuário" : "Senha";
  char shown[FLD_MAX * 3 + 4];
  const char *f = ui->field[idx];
  if (idx == 1 && !ui->show_pass) {
    int n = (int)strlen(f), o = 0;
    for (int i = 0; i < n && o + 4 < (int)sizeof(shown); i++) { shown[o++] = (char)0xE2; shown[o++] = (char)0x80; shown[o++] = (char)0xA2; }
    shown[o] = 0;
  } else {
    snprintf(shown, sizeof(shown), "%s", f);
  }
  int inner = w - 56;
  const char *p = shown;
  while (*p && TW(ui, 0, 30, p, 0) > inner) {                  /* mostra o final do texto se nao couber */
    p++;
    while (((unsigned char)*p & 0xC0) == 0x80) p++;
  }
  int tx = x + 28, base = y + h / 2 + 10;
  if (!f[0]) {
    T(ui, 0, 30, tx, base, ph, C_MUTED, 140, 0);
    if (active && (ui->frame / 30) % 2 == 0) gfx_fill(s, tx, y + 14, 3, h - 28, C_TEXT, 255);
  } else {
    int tw = T(ui, 0, 30, tx, base, p, C_TEXT, 255, 0);
    if (active && (ui->frame / 30) % 2 == 0) gfx_fill(s, tx + tw + 3, y + 14, 3, h - 28, C_TEXT, 255);
  }
}

static void draw_key(UI *ui, int x, int y, int w, int h, const char *label, int sel, int on) {
  Image *s = ui->s;
  gfx_rrect(s, x, y, w, h, 14, on ? C_ACCENT : C_PANEL2, 255, 0);
  if (!on) gfx_rrect(s, x, y, w, h, 14, C_WHITE, 26, 2);
  text_draw_in(s, ui->fb, 28, x, w, y + h / 2 + 10, label, on ? C_DARKTXT : C_TEXT, 255, 0, 1);
  if (sel) gfx_rrect(s, x - 5, y - 5, w + 10, h + 10, 18, C_FOCUS, 255, 4);
}

static void draw_login(UI *ui) {
  Image *s = ui->s;
  gfx_rrect(s, LG_X, LG_Y, LG_W, LG_H, 28, C_PANEL, 245, 0);
  gfx_rrect(s, LG_X, LG_Y, LG_W, LG_H, 28, C_WHITE, 26, 2);
  int px = LG_X + 45;
  if (ui->logo) {
    Image *lg = ui->logo_full ? img_scale(ui->logo_full, 116, 116) : NULL;
    if (lg) { gfx_blit(s, lg, LG_X + LG_W - 45 - 116, LG_Y + 26, 0, 255); img_free(lg); }
  }
  T(ui, 1, 22, px, LG_Y + 62, "ACESSO RESTRITO", C_ACCENT, 255, 4);
  T(ui, 1, 46, px, LG_Y + 122, "Entrar", C_TEXT, 255, 0);

  draw_field(ui, 0, LG_Y + 160);
  draw_field(ui, 1, LG_Y + 236);

  int ky = LG_Y + 332;
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < KB_COLS; c++) {
      char lab[2] = { (ui->caps ? KB_S : KB_N)[r][c], 0 };
      draw_key(ui, px + c * (KEY_W + KEY_G), ky + r * (KEY_H + KEY_G), KEY_W, KEY_H, lab,
               ui->kr == r && ui->kc == c, 0);
    }
  }
  static const char *act[KB_ACTIONS] = {"MAIÚSC", "MOSTRAR", "APAGAR", "CAMPO", "CADASTRAR", "ENTRAR"};
  static const int aw[KB_ACTIONS] = {125, 140, 125, 105, 195, 0};
  int ax = px, ay = ky + 4 * (KEY_H + KEY_G), total = LG_W - 90;
  int used = 0;
  for (int i = 0; i < KB_ACTIONS - 1; i++) used += aw[i] + KEY_G;
  for (int i = 0; i < KB_ACTIONS; i++) {
    int w = aw[i] ? aw[i] : total - used;
    int on = (i == ACT_CAPS && ui->caps) || (i == ACT_SHOW && ui->show_pass);
    int sel = (ui->kr == 4 && ui->kc == i);
    if (i == ACT_GO) {                                           /* botao principal em verde */
      gfx_rrect(s, ax, ay, w, KEY_H, 14, C_BASE, 255, 0);
      gfx_rrect(s, ax, ay, w, KEY_H, 14, C_BASE_EDGE, 255, 2);
      text_draw_in(s, ui->fb, 24, ax, w, ay + KEY_H / 2 + 9, act[i], C_WHITE, 255, 0, 1);
      if (sel) gfx_rrect(s, ax - 5, ay - 5, w + 10, KEY_H + 10, 18, C_FOCUS, 255, 4);
    } else {
      gfx_rrect(s, ax, ay, w, KEY_H, 14, on ? C_ACCENT : C_PANEL2, 255, 0);
      if (!on) gfx_rrect(s, ax, ay, w, KEY_H, 14, C_WHITE, 26, 2);
      text_draw_in(s, ui->fb, 21, ax, w, ay + KEY_H / 2 + 8, act[i], on ? C_DARKTXT : C_TEXT, 255, 0, 1);
      if (sel) gfx_rrect(s, ax - 5, ay - 5, w + 10, KEY_H + 10, 18, C_FOCUS, 255, 4);
    }
    ax += w + KEY_G;
  }
  if (ui->login_msg[0]) {
    char buf[200];
    text_fit(ui->f, 26, ui->login_msg, LG_W - 90, buf, sizeof(buf));
    text_draw_in(s, ui->f, 26, px, LG_W - 90, LG_Y + LG_H - 34, buf, C_ERR, 255, 0, 1);
  }
}

static void draw_qr(UI *ui) {
  Image *s = ui->s;
  gfx_rrect(s, LG_X, LG_Y, LG_W, LG_H, 28, C_PANEL, 245, 0);
  gfx_rrect(s, LG_X, LG_Y, LG_W, LG_H, 28, C_WHITE, 26, 2);
  text_draw_in(s, ui->fb, 44, LG_X, LG_W, LG_Y + 72, "Cadastro enviado!", C_ACCENT, 255, 0, 1);
  char l[160];
  snprintf(l, sizeof(l), "Usuário: %s", ui->qr_user);
  text_draw_in(s, ui->fb, 30, LG_X, LG_W, LG_Y + 122, l, C_TEXT, 255, 0, 1);
  text_draw_in(s, ui->f, 26, LG_X, LG_W, LG_Y + 168, "Sua conta está aguardando ativação.", C_MUTED, 255, 0, 1);
  int q = 450, qx = LG_X + (LG_W - q) / 2, qy = LG_Y + 192;     /* o PNG ja tem 450x450 (10 px por modulo): sem redimensionar */
  gfx_rrect(s, qx - 8, qy - 8, q + 16, q + 16, 18, RGB(255, 255, 255), 255, 0);
  if (ui->qr) {
    gfx_blit(s, ui->qr, qx, qy, 0, 255);
  } else {
    text_draw_in(s, ui->f, 24, qx, q, qy + q / 2, "(QR Code indisponível)", C_DARKTXT, 255, 0, 1);
  }
  text_draw_in(s, ui->fb, 28, LG_X, LG_W, qy + q + 58, "Escaneie com o celular e fale com o admin no WhatsApp", C_TEXT, 255, 0, 1);
  text_draw_in(s, ui->f, 24, LG_X, LG_W, qy + q + 96, "Quando ele ativar sua conta (30 dias), volte e toque em ENTRAR.", C_MUTED, 255, 0, 1);
}

static void draw_loading(UI *ui) {
  Image *s = ui->s;
  int w = 760, h = 260, x = (W - w) / 2, y = 400;
  gfx_rrect(s, x, y, w, h, 28, C_PANEL, 245, 0);
  gfx_rrect(s, x, y, w, h, 28, C_WHITE, 26, 2);
  draw_spinner(ui, W / 2, y + 86);
  char buf[160];
  text_fit(ui->fb, 32, ui->loading_msg, w - 80, buf, sizeof(buf));
  text_draw_in(s, ui->fb, 32, x, w, y + 178, buf, C_TEXT, 255, 0, 1);
}


/* ---- janela do video */
static void fmt_time(char *out, int cap, double sec) {
  int s = (int)sec;
  snprintf(out, (size_t)cap, "%d:%02d", s / 60, s % 60);
}

static void draw_video_overlay(UI *ui) {
  Image *s = ui->s;
  int st = vp_state();
  int win = ui->vid_mode == 1;
  int ax, ay, aw, ah;                                           /* area do video */
  if (win) { ax = 480; ay = 250; aw = 960; ah = 540; }
  else { ax = W - MARGIN - 384; ay = 124; aw = 384; ah = 216; }

  if (win) {
    gfx_rrect(s, 450, 170, 1020, 700, 26, C_PANEL, 250, 0);
    gfx_rrect(s, 450, 170, 1020, 700, 26, C_WHITE, 30, 2);
    char t[120];
    text_fit(ui->fb, 30, ui->vtitle, 940, t, sizeof(t));
    T(ui, 1, 30, 480, 222, t, C_TEXT, 255, 0);
  } else {
    gfx_rrect(s, ax - 6, ay - 6, aw + 12, ah + 12, 14, C_PANEL, 250, 0);
    gfx_rrect(s, ax - 6, ay - 6, aw + 12, ah + 12, 14, C_ACCENT, 200, 2);
  }
  gfx_fill(s, ax, ay, aw, ah, C_BLACK, 255);

  int vw = 0, vh = 0;
  if (ui->vframe && vp_aspect(&vw, &vh)) {
    int dw = aw, dh = aw * vh / vw;
    if (dh > ah) { dh = ah; dw = ah * vw / vh; }
    if (dw > 0 && dh > 0) gfx_blit_scaled(s, ui->vframe, ax + (aw - dw) / 2, ay + (ah - dh) / 2, dw, dh);
  }

  char msg[200] = "";
  if (st == VP_DOWNLOADING) snprintf(msg, sizeof(msg), "Carregando vídeo...  %d%%", vp_progress());
  else if (st == VP_ERROR) vp_error(msg, sizeof(msg));
  else if (st == VP_ENDED) snprintf(msg, sizeof(msg), "Fim do vídeo");
  else if (st == VP_PLAYING && vp_paused()) snprintf(msg, sizeof(msg), "Pausado");
  if (msg[0]) {
    if (st != VP_DOWNLOADING) gfx_fill(s, ax, ay, aw, ah, C_BLACK, st == VP_ERROR ? 255 : 120);
    char b[200];
    text_fit(ui->fb, win ? 34 : 20, msg, aw - 40, b, sizeof(b));
    text_draw_in(s, ui->fb, win ? 34 : 20, ax, aw, ay + ah / 2 + (win ? 12 : 7), b, st == VP_ERROR ? C_ERR : C_TEXT, 255, 0, 1);
    if (st == VP_DOWNLOADING) {
      int bw = aw - 120 > 0 ? aw - 120 : aw;
      int by = ay + ah / 2 + (win ? 50 : 22);
      gfx_rrect(s, ax + (aw - bw) / 2, by, bw, win ? 10 : 6, 4, C_WHITE, 40, 0);
      int fw = bw * vp_progress() / 100;
      if (fw > 6) gfx_rrect(s, ax + (aw - bw) / 2, by, fw, win ? 10 : 6, 4, C_ACCENT, 255, 0);
    }
  }

  double dur = vp_duration(), pos = vp_position();
  if (dur > 0 && (st == VP_PLAYING || st == VP_ENDED)) {
    int fw = (int)((double)aw * (pos > dur ? dur : pos) / dur);
    gfx_fill(s, ax, ay + ah + (win ? 14 : 0), aw, win ? 8 : 5, C_WHITE, 50);
    if (fw > 0) gfx_fill(s, ax, ay + ah + (win ? 14 : 0), fw, win ? 8 : 5, C_ACCENT, 255);
  }
  if (win) {
    char a[24], b[24], tm[64];
    fmt_time(a, sizeof(a), pos);
    fmt_time(b, sizeof(b), dur);
    snprintf(tm, sizeof(tm), "%s / %s", a, b);
    T(ui, 0, 24, 480, 842, tm, C_MUTED, 255, 0);
    const char *hint_txt = st == VP_ENDED ? "X repetir    \xE2\x96\xB3 minimizar    O fechar"
                         : st == VP_DOWNLOADING ? "\xE2\x96\xB3 minimizar    O cancelar"
                         : "X pausar    \xE2\x96\xB3 minimizar    O fechar";
    int hw = TW(ui, 0, 24, hint_txt, 0);
    T(ui, 0, 24, 1440 - hw, 842, hint_txt, C_TEXT, 255, 0);
  } else {
    text_draw_in(s, ui->f, 16, ax, aw, ay + ah - 12, "OPTIONS ampliar   \xE2\x96\xB3 fechar", C_TEXT, 220, 0, 0);
  }
}


static void draw_scene(UI *ui);
/* ------------------------------------------------------------------ abertura animada */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
/* ease-out cubico, entrada/saida 0..1000 */
static int ease(int p) { int q = 1000 - clampi(p, 0, 1000); return 1000 - (int)((long)q * q / 1000 * q / 1000); }

static void draw_splash_content(UI *ui, int t, int amul) {      /* amul: 0..255 (some no fim) */
  Image *s = ui->s;
  int cx = W / 2, cy = H / 2 - 60, R = 170;
  int e_logo = ease((t - 6) * 1000 / 42), e_txt = ease((t - 40) * 1000 / 40);
  int slide = 26 - 26 * e_logo / 1000;
  if (ui->splash_logo) {
    gfx_blit(s, ui->splash_logo, cx - ui->splash_logo->w / 2, cy - ui->splash_logo->h / 2 + slide, 0,
             (255 * e_logo / 1000) * amul / 255);
  } else {
    text_draw_in(s, ui->fb, 96, 0, W, cy + 30, "JK", C_ACCENT, (255 * e_logo / 1000) * amul / 255, 4, 1);
  }
  int a = (255 * e_txt / 1000) * amul / 255;
  text_draw_in(s, ui->fb, 44, 0, W, cy + R + 100 + (18 - 18 * e_txt / 1000), "Os melhores Patchs de PS4", C_TEXT, a, 2, 1);
}

/* O fundo e a logo sao preparados uma vez; por quadro so ha uma copia + a logo + o texto (leve para o PS4). */
static void draw_splash(UI *ui) {
  int t = ui->splash;
  if (!ui->splash_base) {
    ui->splash_base = img_new(W, H);
    if (ui->splash_base) {
      if (ui->bg) gfx_blit_surface(ui->splash_base, ui->bg); else gfx_fill(ui->splash_base, 0, 0, W, H, C_BG, 255);
      gfx_fill(ui->splash_base, 0, 0, W, H, C_BG, 120);
    }
    if (ui->logo_full) ui->splash_logo = img_scale(ui->logo_full, 340, 340);
  }
  if (ui->splash_base) gfx_blit_surface(ui->s, ui->splash_base);
  else gfx_fill(ui->s, 0, 0, W, H, C_BG, 255);
  int k = t < SPLASH_FADE ? 255 : clampi((SPLASH_END - t) * 255 / (SPLASH_END - SPLASH_FADE), 0, 255);
  draw_splash_content(ui, t, k);
}

void ui_skip_splash(UI *ui) { ui->splash = SPLASH_END - 1; ui_tick(ui); ui->dirty = 1; }

static void draw_scene(UI *ui) {
  if (ui->bg) gfx_blit_surface(ui->s, ui->bg);
  else gfx_fill(ui->s, 0, 0, W, H, C_BG, 255);
  draw_topbar(ui);
  if (ui->mode == MODE_CATALOG) {
    draw_heading(ui);
    draw_chips(ui);
    draw_cards(ui);
    draw_panel(ui);
    draw_message_line(ui);
  } else if (ui->mode == MODE_LOGIN) {
    draw_login(ui);
  } else if (ui->mode == MODE_QR) {
    draw_qr(ui);
  } else {
    draw_loading(ui);
  }
  draw_footer(ui);
}

void ui_draw(UI *ui) {
  if (ui->splash < SPLASH_END) { draw_splash(ui); return; }
  if (!(ui->vid_mode && ui->mode == MODE_CATALOG)) { draw_scene(ui); return; }
  /* video ativo: o catalogo fica guardado em 'base' e so a janela do video e redesenhada a cada quadro */
  if (!ui->base) ui->base = img_new(W, H);
  if (ui->base && (ui->base_stale || ui->dirty)) {
    draw_scene(ui);
    if (ui->vid_mode == 1) gfx_fill(ui->s, 0, 0, W, H, C_BLACK, 150);
    gfx_blit_surface(ui->base, ui->s);
    ui->base_stale = 0;
  } else if (ui->base) {
    gfx_blit_surface(ui->s, ui->base);
  } else {
    draw_scene(ui);
  }
  draw_video_overlay(ui);
}
