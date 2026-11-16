#include "ui.h"
#include "auth_proto.h"
#include "qrcode.h"
#include "plans_default.h"
#include <time.h>
#include "version.h"
#include "vplayer.h"

#include <stdio.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
#define NTH          4                  /* tamanhos de capa: 0 = em foco, 1..3 = vizinhas cada vez menores */
static const int TH_W[NTH] = {340, 250, 200, 160};
static const int TH_H[NTH] = {573, 421, 337, 270};
#define CAR_Y        335                /* topo da capa em foco na vitrine */
#define CAR_GAP      30
#define DT_X         MARGIN             /* tela de detalhe: coluna da esquerda */
#define DT_W         700
#define DT_PX        850                /* painel de downloads */
#define DT_PY        140
#define DT_PW        974
#define DT_PH        820
#define BTN_H        72
#define BTN_GAP      10
#define DL_VIS       8                  /* downloads visiveis ao mesmo tempo */
#define MD_VIS       2                  /* videos/imagens visiveis ao mesmo tempo */
static int g_bh = BTN_H;                /* altura do botao em uso */
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
  UiPreviewFn preview_fn;              /* botao "Ver a loja" (visitante) */
  int offline;                         /* 1 = mostrando o catalogo salvo porque o servidor nao respondeu */
  int locked, visitor;                 /* catalogo so para olhar: sem downloads (visitante ou plano vencido/pendente) */
  Image *qr;                           /* QR Code do WhatsApp do admin (assets/images/qr_whatsapp.png) */
  int shop;                            /* janela de planos: 0 fechada, 1 escolher plano, 2 Pix, 3 WhatsApp */
  int shop_sel;                        /* plano em foco */
  AuthPlans plans;                     /* precos e Pix (embutidos; o servidor atualiza) */
  UiReloadFn reload_fn;                /* pede ao app para reler o plano no servidor */
  char pi_state[16], pi_name[16];      /* plano atual (servidor novo): estado e nome */
  long pi_exp, pi_now;                 /* vencimento e hora do servidor quando foi lido (s) */
  time_t pi_t0;                        /* hora local da leitura: o contador do topo anda sozinho */
  int pi_on;                           /* 1 = o topo mostra o plano calculado aqui */
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
  char plan[96];                       /* texto do plano no topo (vazio = nao mostra) */
  int plan_level;                      /* 0 normal, 1 aviso (perto de vencer), 2 vencido */
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
  Image *fbg;                          /* fundo da capa em foco (estadio + capa ampliada e borrada) */
  Image *th[CAT_MAX_GAMES][NTH];       /* capas em varios tamanhos */
  Image *qr_small;                     /* QR pequeno da tela de login */
  int detail;                          /* 0 = vitrine de capas, 1 = detalhe do jogo (downloads + videos) */
  int zone;                            /* detalhe: 0 = downloads, 1 = videos/imagens */
  int dfocus, dscroll, dscroll2, mfocus, mscroll;   /* dscroll2: rolagem da 2a coluna (plugins) no modo dividido */
  Image *fbgc[CAT_MAX_GAMES];          /* fundos prontos (1 por capa): trocar de card so copia a imagem, sem refazer o desfoque */
  int idle;                            /* quadros desde o ultimo botao (so prepara os fundos quando o usuario para) */
  int fbg_due;                         /* 1 = refazer o fundo borrado assim que a animacao de troca de capa terminar */
  int slide;                           /* vitrine: deslocamento (px) das capas durante a animacao de troca; 0 = parado */

  int filt[CAT_MAX_GAMES], nfilt;
  int cat_idx, sel, scroll, focus;

  int busy, dirty;
  int auto_upd;                        /* 1 = ao receber o catalogo, atualiza o app sozinho (uma vez por abertura) */
  int updating;                        /* o pedido em andamento e a atualizacao do proprio app */
  int upd_ready;                       /* 1 = o PKG novo do app ja esta baixado em /data/pkg */
  int upd_help;                        /* 1 = tela com o passo a passo da instalacao aberta */
  char upd_file[96];                   /* nome do PKG baixado (mostrado no passo a passo) */
  unsigned frame;
  char toast[256];
  int toast_left, toast_kind;
  char assets[256];

  /* video dentro do app: 0 = nenhum, 1 = janela, 2 = minimizado (PiP) */
  int vid_mode, vdirty, base_stale, vlast_state, vlast_pct, vlast_sec;
  char vtitle[96], vurl[240];
  Image *vframe, *base;

  /* guia (imagem) dentro do app: 0 = fechado, 1 = baixando, 2 = mostrando */
  int gmode, gserial, gscroll;
  volatile int gstate;                 /* resultado do download: 0 em andamento, 1 ok, -1 falhou */
  Image *gimg;                         /* imagem ja ajustada ao tamanho da tela */
  char gtitle[96], gpath[320];
};

/* ------------------------------------------------------------------ utilidades */
/* Pastas: categorias que so aparecem ao abrir o card-pasta (tag "PASTA") de mesmo nome. "plugins" e "jkmod" (v1.16). */
static int is_folder_cat(const char *id) { return !strcmp(id, "plugins") || !strncmp(id, "jkmod", 5); }   /* jkmod_mega, jkmod_bras (v1.17) */

static void refilter(UI *ui) {
  ui->nfilt = 0;
  if (!ui->cat) return;
  const char *id = ui->cat->cat[ui->cat_idx].id;
  for (int i = 0; i < ui->cat->ngame; i++) {
    /* em "Todos" os cards da categoria "plugins" ficam escondidos: so aparecem ao abrir o card-pasta "Plugins & Mods" */
    if (ui->cat_idx == 0 ? !is_folder_cat(ui->cat->game[i].category) : !strcmp(ui->cat->game[i].category, id)) ui->filt[ui->nfilt++] = i;
  }
}

static const CatGame *cur_game(UI *ui) {
  if (!ui->cat || ui->nfilt <= 0) return NULL;
  return &ui->cat->game[ui->filt[ui->sel]];
}

/* Itens do detalhe: "downloads" = botoes pkg/download; "midias" = botoes video/image e, por ultimo, o video do card
 * (indice nbtn). dl[] e md[] guardam o indice do botao dentro de g->btn. */
static int is_media(const CatButton *b) { return !strcmp(b->action, "video") || !strcmp(b->action, "image"); }
/* Modo dividido (card "APPs e Plugins", tag APPS): detalhe em 3 colunas = info | APPs (PKG, verde) | PLUGINS (arquivos, amarelo). */
static int split_mode(const CatGame *g) { return g && !strcmp(g->tag, "APPS"); }
static int is_pkg_btn(const CatButton *b) { return !is_media(b) && !strcmp(b->action, "pkg"); }
static int split_na(const CatGame *g) { int n = 0; if (!split_mode(g)) return 0; for (int i = 0; i < g->nbtn; i++) if (is_pkg_btn(&g->btn[i])) n++; return n; }
static void lists(const CatGame *g, int *dl, int *nd, int *md, int *nm) {
  *nd = 0; *nm = 0;
  if (!g) return;
  if (split_mode(g)) {                                            /* PKGs primeiro, depois os demais downloads */
    for (int i = 0; i < g->nbtn; i++) if (is_pkg_btn(&g->btn[i])) dl[(*nd)++] = i;
    for (int i = 0; i < g->nbtn; i++) if (!is_media(&g->btn[i]) && !is_pkg_btn(&g->btn[i])) dl[(*nd)++] = i;
    for (int i = 0; i < g->nbtn; i++) if (is_media(&g->btn[i])) md[(*nm)++] = i;
  } else
  for (int i = 0; i < g->nbtn; i++) {
    if (is_media(&g->btn[i])) md[(*nm)++] = i;
    else dl[(*nd)++] = i;
  }
  if (g->video[0]) md[(*nm)++] = g->nbtn;
}

static void rebuild_fbg(UI *ui);

static void select_game(UI *ui, int idx) {
  ui->slide = 0;
  ui->sel = idx;
  ui->detail = 0;
  ui->dirty = 1;
  ui->fbg_due = 0;
  { const CatGame *cg = cur_game(ui);
    if (cg && ui->fbgc[ui->filt[ui->sel]] && ui->fbg) gfx_blit_surface(ui->fbg, ui->fbgc[ui->filt[ui->sel]]);   /* fundo ja pronto: so copia */
    else ui->fbg_due = 1; }                                                   /* senao refaz no ui_tick, depois da animacao */
}

static void type_label(const char *type, char *out, int cap) {
  if (!strcmp(type, "PS4GD")) snprintf(out, cap, "JOGO");
  else if (!strcmp(type, "PS4GP")) snprintf(out, cap, "PATCH");
  else if (!strcmp(type, "PS4AC")) snprintf(out, cap, "DLC");
  else out[0] = 0;
}

static void info_text(const CatButton *b, char *out, int cap) {
  if (!strcmp(b->action, "download")) { snprintf(out, cap, "ARQUIVO"); return; }
  if (!strcmp(b->action, "video")) { snprintf(out, cap, "VÍDEO"); return; }
  if (!strcmp(b->action, "image")) { snprintf(out, cap, "IMAGEM"); return; }
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

static void free_thumbs(UI *ui, int i) {
  for (int k = 0; k < NTH; k++) { if (ui->th[i][k]) { img_free(ui->th[i][k]); ui->th[i][k] = NULL; } }
  if (ui->fbgc[i]) { img_free(ui->fbgc[i]); ui->fbgc[i] = NULL; }
}

/* Carrega a capa do arquivo e gera os 4 tamanhos. Retorna 1 se deu certo. */
static int thumbs_from_file(UI *ui, int i, const char *path) {
  Image *raw = img_load(path);
  if (!raw) return 0;
  Image *t[NTH];
  int ok = 1;
  for (int k = 0; k < NTH; k++) { t[k] = img_scale_cover(raw, TH_W[k], TH_H[k]); if (!t[k]) ok = 0; }
  img_free(raw);
  if (!ok) { for (int k = 0; k < NTH; k++) if (t[k]) img_free(t[k]); return 0; }
  free_thumbs(ui, i);
  for (int k = 0; k < NTH; k++) ui->th[i][k] = t[k];
  return 1;
}

/* Fundo da vitrine: estadio + a capa em foco ampliada e borrada (miniatura esticada com filtro bilinear). */
static void compose_fbg(UI *ui, Image *dst, int gi) {
  gfx_blit_surface(dst, ui->bg);
  Image *t = gi >= 0 ? ui->th[gi][0] : NULL;
  if (t) {
    Image *tiny = img_scale(t, 64, 108);
    Image *bl = tiny ? img_new(W, H) : NULL;
    if (tiny && bl) {
      Image crop;                                             /* faixa do meio da capa, na proporcao da tela */
      crop.w = 64; crop.h = 36; crop.px = tiny->px + (size_t)20 * 64;
      gfx_blit_scaled(bl, &crop, 0, 0, W, H);
      gfx_blit(dst, bl, 0, 0, 0, 150);
    }
    if (bl) img_free(bl);
    if (tiny) img_free(tiny);
  }
  gfx_vfade(dst, 0, 0, W, H, C_BG, 60, 215);
}

static void rebuild_fbg(UI *ui) {
  if (!ui->bg) return;
  if (!ui->fbg) ui->fbg = img_new(W, H);
  if (!ui->fbg) return;
  const CatGame *g = cur_game(ui);
  int gi = g ? ui->filt[ui->sel] : -1;
  if (gi >= 0 && ui->fbgc[gi]) { gfx_blit_surface(ui->fbg, ui->fbgc[gi]); return; }
  compose_fbg(ui, ui->fbg, gi);
  if (gi >= 0 && ui->th[gi][0]) {                              /* guarda para as proximas trocas */
    Image *c = img_new(W, H);
    if (c) { gfx_blit_surface(c, ui->fbg); ui->fbgc[gi] = c; }
  }
}

static void load_thumbs(UI *ui) {
  char path[320];
  pthread_mutex_lock(&cover_mu);
  ui->cover_gen++;
  for (int i = 0; i < CAT_MAX_GAMES; i++) ui->cover_ready[i] = 0;
  ui->cover_pending = 0;
  int gen = ui->cover_gen;
  pthread_mutex_unlock(&cover_mu);
  for (int i = 0; i < CAT_MAX_GAMES; i++) free_thumbs(ui, i);
  if (!ui->cat) return;
  CoverJobs *J = NULL;
  for (int i = 0; i < ui->cat->ngame; i++) {
    const char *cv = ui->cat->game[i].cover;
    if (!cv[0]) continue;
    if (is_remote_cover(cv)) {
      if (!ui->cover_fetch || !ui->cover_dir[0]) continue;
      cover_cache_path(ui, cv, path, sizeof(path));
      thumbs_from_file(ui, i, path);                            /* ja baixada antes? */
      if (!ui->th[i][0]) {
        const char *base = strrchr(cv, '/'); base = base ? base + 1 : cv;       /* capa de reserva dentro do app, se tiver */
        char nm[96]; snprintf(nm, sizeof(nm), "%s", base);
        char *q = strchr(nm, '?'); if (q) *q = 0;
        char bp[512]; snprintf(bp, sizeof(bp), "%s/images/%s", ui->assets, nm);
        thumbs_from_file(ui, i, bp);
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
    thumbs_from_file(ui, i, path);
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
    if (thumbs_from_file(ui, i, path)) { ui->dirty = 1; if (ui->nfilt > 0 && ui->filt[ui->sel] == i) rebuild_fbg(ui); }
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
    if (ui->qr) ui->qr_small = img_scale(ui->qr, 180, 180);
    auth_parse_plans(PLANS_DEFAULT_BODY, &ui->plans);
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
  rebuild_fbg(ui);
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
void ui_set_preview_handler(UI *ui, UiPreviewFn fn) { ui->preview_fn = fn; }

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
  ui->plan[0] = 0;
  ui->dirty = 1;
}

void ui_set_user(UI *ui, const char *name) {
  snprintf(ui->username, sizeof(ui->username), "%s", name ? name : "");
  ui->dirty = 1;
}

void ui_set_offline(UI *ui, int on) { ui->offline = on; ui->dirty = 1; }

void ui_set_reload_handler(UI *ui, UiReloadFn fn) { ui->reload_fn = fn; }

void ui_open_shop(UI *ui) {
  if (ui->mode != MODE_CATALOG || ui->visitor || !ui->username[0]) return;
  ui->shop = 1;
  if (ui->shop_sel < 0 || ui->shop_sel >= ui->plans.n) ui->shop_sel = 0;
  ui->dirty = 1;
}

void ui_remind_support(UI *ui) { ui_open_shop(ui); }

void ui_set_plans_text(UI *ui, const char *body) {
  AuthPlans p;
  if (!body || !body[0] || auth_parse_plans(body, &p) != 0) return;
  ui->plans = p;
  if (ui->shop_sel >= ui->plans.n) ui->shop_sel = 0;
  ui->dirty = 1;
}

/* Recalcula o texto do plano no topo com a hora do servidor + o tempo passado desde a leitura. */
static void plan_refresh(UI *ui) {
  if (!ui->pi_on) return;
  long now = ui->pi_now > 0 ? ui->pi_now + (long)(time(NULL) - ui->pi_t0) : 0;
  char t[96];
  int lv = auth_plan_status(ui->pi_state, ui->pi_name, ui->pi_exp, now, t, sizeof(t));
  if (strcmp(t, ui->plan) != 0 || lv != ui->plan_level) {
    snprintf(ui->plan, sizeof(ui->plan), "%s", t);
    ui->plan_level = lv;
    ui->dirty = 1;
  }
  if (ui->pi_exp > 0 && now >= 1700000000L && now >= ui->pi_exp && !ui->locked && !ui->visitor && ui->mode == MODE_CATALOG) {
    ui->locked = 1;                                       /* venceu com o app aberto: trava na hora */
    ui->detail = 0;
    ui_toast(ui, "Seu acesso terminou. Assine um plano para continuar baixando.", UI_TOAST_ERR);
    ui_open_shop(ui);
  }
}

void ui_set_plan_info(UI *ui, const char *state, const char *plan_name, long expires, long server_now) {
  snprintf(ui->pi_state, sizeof(ui->pi_state), "%s", state ? state : "");
  snprintf(ui->pi_name, sizeof(ui->pi_name), "%s", plan_name ? plan_name : "");
  ui->pi_exp = expires;
  ui->pi_now = server_now;
  ui->pi_t0 = time(NULL);
  ui->pi_on = 1;
  plan_refresh(ui);
}

void ui_set_plan(UI *ui, const char *text, int level) {
  ui->pi_on = 0;
  snprintf(ui->plan, sizeof(ui->plan), "%s", text ? text : "");
  ui->plan_level = level;
  ui->dirty = 1;
}

void ui_set_catalog(UI *ui, const Catalog *cat) {
  ui->auto_upd = 1;
  ui->upd_ready = 0;
  ui->upd_help = 0;
  ui->cat = cat;
  ui->shop = 0;
  load_thumbs(ui);
  ui->cat_idx = 0;
  refilter(ui);
  select_game(ui, 0);
  ui->mode = MODE_CATALOG;
  ui->busy = 0;
  ui->dirty = 1;
  ui->locked = cat && cat->locked;
  ui->visitor = ui->locked && !ui->username[0];
  if (ui->offline) ui_toast(ui, "Servidor fora do ar: mostrando a última loja salva. Os downloads podem não funcionar agora.", UI_TOAST_INFO);
  else if (ui->visitor) ui_toast(ui, "Modo visitante: veja tudo, mas para baixar é preciso ativar o plano.", UI_TOAST_INFO);
  else if (ui->locked) ui_toast(ui, "Assine um plano para baixar. Vídeos e tutoriais continuam liberados.", UI_TOAST_INFO);
  if (ui->locked && !ui->visitor && !ui->offline) ui_open_shop(ui);       /* bloqueado: ja abre a janela de planos */
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

static void preview_start(UI *ui) {
  if (!ui->preview_fn) return;
  ui->username[0] = 0;
  ui_show_loading(ui, "Abrindo a loja...");
  ui->preview_fn(ui->auth_ud);
}

static void login_key(UI *ui, int key) {
  if (ui->kr < 0) {                                              /* foco no botao "Ver a loja" (lado esquerdo) */
    if (key == UI_OK) preview_start(ui);
    else if (key == UI_RIGHT || key == UI_DOWN || key == UI_BACK || key == UI_SQUARE) { ui->kr = key == UI_DOWN ? 1 : 0; ui->kc = 0; ui->dirty = 1; }
    else if (key == UI_OPTIONS) login_submit(ui);
    return;
  }
  switch (key) {
    case UI_LEFT:  if (ui->kc > 0) { ui->kc--; ui->dirty = 1; } else if (ui->preview_fn) { ui->kr = -1; ui->dirty = 1; } break;
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

void ui_app_pkg_name(const char *url, char *out, size_t n) {
  const char *base = strrchr(url, '/');
  base = base ? base + 1 : url;
  size_t k = 0;
  while (*base && *base != '?' && k + 1 < n) {
    unsigned char ch = (unsigned char)*base++;
    if (ch == '%' && base[0] && base[1]) {
      char hex[3] = { base[0], base[1], 0 };
      char *end = NULL; long v = strtol(hex, &end, 16);
      if (end && *end == 0) { ch = (unsigned char)v; base += 2; }
    }
    out[k++] = (isalnum(ch) || ch == '.' || ch == '_' || ch == '-') ? (char)ch : '_';
  }
  out[k] = 0;
  if (!out[0] || !strcmp(out, ".") || !strcmp(out, "..")) snprintf(out, n, "JKPATCHS_NOVO.pkg");
}

/* Atualizacao do app: o PS4 nao instala um app por cima de outro, entao o app so BAIXA o PKG novo em /data/pkg
 * e mostra o passo a passo (apagar o app e instalar pelo Package Installer do GoldHEN). O login continua salvo. */
static void trigger_update(UI *ui) {
  if (!update_available(ui)) { ui_toast(ui, "Seu app já está na versão mais recente.", UI_TOAST_INFO); return; }
  if (ui->upd_ready) { ui->upd_help = 1; ui->dirty = 1; return; }       /* ja baixado: mostra como instalar */
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
  snprintf(b.action, sizeof(b.action), "appupdate");
  ui_app_pkg_name(a->url, ui->upd_file, sizeof(ui->upd_file));
  ui_toast(ui, "Baixando a atualização do app...", UI_TOAST_INFO);
  ui->busy = 1;
  ui->updating = 1;
  if (ui->fn) ui->fn(&b, ui->user);
}

void ui_install_done(UI *ui, int code, const char *err) {
  ui_set_busy(ui, 0);
  if (ui->updating) {                                         /* terminou o download da atualizacao do app */
    ui->updating = 0;
    if (code == 2) {
      ui->upd_ready = 1;
      ui_toast(ui, "Nova versão do app baixada! Aperte  \xE2\x96\xA1  para ver como instalar.", UI_TOAST_OK);
    } else {
      ui_toast(ui, (err && err[0]) ? err : "Não foi possível baixar a atualização do app.", UI_TOAST_ERR);
    }
    ui->dirty = 1;
    return;
  }
  if (code == 1)
    ui_toast(ui, (err && err[0]) ? err : "Download concluído!", UI_TOAST_OK);
  else if (code == 0 && ui->updating)
    ui_toast(ui, "Atualização enviada! Feche o app (botão PS > Fechar), espere terminar nas Notificações e abra de novo.", UI_TOAST_OK);
  else if (code == 0)
    ui_toast(ui, "Pedido aceito! Acompanhe nas Notificações do PS4.", UI_TOAST_OK);
  else
    ui_toast(ui, (err && err[0]) ? err : "Falha ao enviar o pedido.", UI_TOAST_ERR);
  ui->updating = 0;
}

static int is_jkv(const char *u);
static void guide_open(UI *ui, const char *title, const char *url);
static void vid_open(UI *ui, const CatGame *g);
static void vid_open_url(UI *ui, const char *title, const char *url);

static void trigger_install(UI *ui) {
  const CatGame *g = cur_game(ui);
  if (!g) return;
  if (ui->focus >= g->nbtn) {                                   /* botao de video */
    if (g->video[0] && is_jkv(g->video)) { vid_open(ui, g); return; }          /* player dentro do app */
    if (g->video[0] && ui->open_fn) {
      ui_toast(ui, "Abrindo o vídeo no navegador do PS4...", UI_TOAST_INFO);
      ui->open_fn(g->video, ui->open_ud);
    }
    return;
  }
  const CatButton *b = &g->btn[ui->focus];
  const char *action = b->action[0] ? b->action : "pkg"; /* catalogos antigos continuam compativeis */
  if (ui->locked && strcmp(action, "video") && strcmp(action, "image")) {
    ui_toast(ui, ui->offline ? "Sem conexão com o servidor. Downloads indisponíveis agora." : "Ative seu plano para baixar.", UI_TOAST_ERR);
    return;
  }
  if (!b->url[0]) { ui_toast(ui, "URL não configurada neste botão.", UI_TOAST_ERR); return; }
  if (!strcmp(action, "video")) {
    if (is_jkv(b->url)) vid_open_url(ui, b->name[0] ? b->name : g->title, b->url);
    else if (ui->open_fn) { ui_toast(ui, "Abrindo o vídeo no navegador do PS4...", UI_TOAST_INFO); ui->open_fn(b->url, ui->open_ud); }
    return;
  }
  if (!strcmp(action, "image")) {
    guide_open(ui, b->name[0] ? b->name : g->title, b->url);       /* visualizador dentro do app */
    return;
  }
  if (ui->busy) { ui_toast(ui, "Já existe um pedido em andamento. Aguarde.", UI_TOAST_INFO); return; }
  if (!strcmp(action, "pkg") && !b->content_id[0]) {
    ui_toast(ui, "Este PKG está sem ID (data-content-id).", UI_TOAST_ERR);
    return;
  }
  if (strcmp(action, "pkg") && strcmp(action, "download")) {
    ui_toast(ui, "Tipo de ação não reconhecido neste botão.", UI_TOAST_ERR);
    return;
  }
  ui_toast(ui, !strcmp(action, "download") ? "Baixando arquivo para o armazenamento do PS4..." : "Enviando pedido ao instalador nativo...", UI_TOAST_INFO);
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

static void vid_open_url(UI *ui, const char *title, const char *url) {
  if (vp_busy()) { ui_toast(ui, "Aguarde o vídeo anterior terminar de fechar e tente de novo.", UI_TOAST_INFO); return; }
  snprintf(ui->vtitle, sizeof(ui->vtitle), "%s", title ? title : "Vídeo");
  snprintf(ui->vurl, sizeof(ui->vurl), "%s", url);
  if (vp_load(url) != 0) { ui_toast(ui, "Não foi possível iniciar o vídeo.", UI_TOAST_ERR); return; }
  ui->vid_mode = 1;
  ui->vlast_state = -1;
  ui->vlast_pct = -1;
  ui->vlast_sec = -1;
  ui->base_stale = 1;
  ui->dirty = 1;
}

static void vid_open(UI *ui, const CatGame *g) {
  if (g) vid_open_url(ui, g->title, g->video);
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

/* ------------------------------------------------------------------ guia (imagem) dentro do app
 * O botao de acao "image" baixa a imagem (fica guardada no console, como as capas) e a mostra em tela cheia.
 * Imagem alta (tutorial): largura fixa e rolagem com as setas. Imagem larga: cabe inteira. O volta fecha.
 * Para trocar a imagem: suba a nova no R2 com outro nome (ou ...png?v=2) e troque o link no catalogo. */
#define G_TOP    112
#define G_VIEW_H (H - G_TOP - 24)
typedef struct { UI *ui; int serial; char url[300], dest[320]; } GuideJob;

static void *guide_worker(void *arg) {
  GuideJob *J = (GuideJob *)arg;
  UI *ui = J->ui;
  char tmp[340];
  snprintf(tmp, sizeof(tmp), "%s.tmp", J->dest);
  int ok = ui->cover_fetch && ui->cover_fetch(J->url, tmp, ui->cover_ud) == 0 && rename(tmp, J->dest) == 0;
  if (!ok) remove(tmp);
  pthread_mutex_lock(&cover_mu);
  if (ui->gserial == J->serial) ui->gstate = ok ? 1 : -1;      /* se o usuario ja fechou, o resultado e ignorado */
  pthread_mutex_unlock(&cover_mu);
  free(J);
  return NULL;
}

static int guide_load(UI *ui) {
  Image *raw = img_load(ui->gpath);
  if (!raw) return -1;
  int tw, th;
  if ((long long)raw->h * 4 > (long long)raw->w * 5) {            /* alta: largura fixa, rola para baixo */
    tw = 1500;
    th = (int)((long long)raw->h * tw / raw->w);
    if (th > 9000) { th = 9000; tw = (int)((long long)raw->w * th / raw->h); }
  } else if ((long long)raw->w * G_VIEW_H > (long long)raw->h * 1840) {   /* larga: cabe inteira */
    tw = 1840; th = (int)((long long)raw->h * tw / raw->w);
  } else {
    th = G_VIEW_H; tw = (int)((long long)raw->w * th / raw->h);
  }
  if (tw < 1) tw = 1;
  if (th < 1) th = 1;
  Image *g = raw;
  if (tw != raw->w || th != raw->h) { g = img_scale(raw, tw, th); img_free(raw); }
  if (!g) return -1;
  if (ui->gimg) img_free(ui->gimg);
  ui->gimg = g;
  ui->gscroll = 0;
  return 0;
}

static void guide_close(UI *ui) {
  if (!ui->gmode) return;
  pthread_mutex_lock(&cover_mu);
  ui->gserial++;                                                  /* download em andamento deixa de valer */
  pthread_mutex_unlock(&cover_mu);
  ui->gmode = 0;
  if (ui->gimg) { img_free(ui->gimg); ui->gimg = NULL; }
  ui->dirty = 1;
}

static void guide_open(UI *ui, const char *title, const char *url) {
  if (!ui->cover_fetch || !ui->cover_dir[0]) {                    /* sem como baixar aqui: cai no navegador */
    if (ui->open_fn) { ui_toast(ui, "Abrindo o guia no navegador do PS4...", UI_TOAST_INFO); ui->open_fn(url, ui->open_ud); }
    return;
  }
  vid_close(ui);
  snprintf(ui->gtitle, sizeof(ui->gtitle), "%s", title ? title : "Guia");
  cover_cache_path(ui, url, ui->gpath, sizeof(ui->gpath));
  pthread_mutex_lock(&cover_mu);
  int serial = ++ui->gserial;
  ui->gstate = 0;
  pthread_mutex_unlock(&cover_mu);
  ui->dirty = 1;
  if (guide_load(ui) == 0) { ui->gmode = 2; return; }              /* ja estava guardada */
  remove(ui->gpath);
  GuideJob *J = (GuideJob *)calloc(1, sizeof(GuideJob));
  if (!J) { ui_toast(ui, "Sem memória para abrir o guia.", UI_TOAST_ERR); return; }
  J->ui = ui; J->serial = serial;
  snprintf(J->url, sizeof(J->url), "%s", url);
  snprintf(J->dest, sizeof(J->dest), "%s", ui->gpath);
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int r = pthread_create(&t, &at, guide_worker, J);
  pthread_attr_destroy(&at);
  if (r != 0) { free(J); ui_toast(ui, "Não foi possível iniciar o download do guia.", UI_TOAST_ERR); return; }
  ui->gmode = 1;
}

static void guide_key(UI *ui, int key) {
  if (key == UI_BACK || key == UI_TRIANGLE) { guide_close(ui); return; }
  if (ui->gmode != 2 || !ui->gimg) return;
  int maxs = ui->gimg->h - G_VIEW_H;
  if (maxs < 0) maxs = 0;
  int d = 0;
  if (key == UI_DOWN) d = 110;
  else if (key == UI_UP) d = -110;
  else if (key == UI_R1) d = G_VIEW_H - 80;
  else if (key == UI_L1) d = -(G_VIEW_H - 80);
  if (!d) return;
  int n = ui->gscroll + d;
  if (n < 0) n = 0;
  if (n > maxs) n = maxs;
  if (n != ui->gscroll) { ui->gscroll = n; ui->dirty = 1; }
}

static void guide_tick(UI *ui) {
  if (!ui->gmode) return;
  if (ui->mode != MODE_CATALOG) { guide_close(ui); return; }      /* saiu da conta / catalogo recarregou */
  if (ui->gmode != 1) return;
  pthread_mutex_lock(&cover_mu);
  int st = ui->gstate;
  pthread_mutex_unlock(&cover_mu);
  if (st == 0) { if (ui->frame % 6 == 0) ui->dirty = 1; return; }  /* gira o indicador */
  if (st == 1 && guide_load(ui) == 0) { ui->gmode = 2; ui->dirty = 1; return; }
  remove(ui->gpath);
  ui->gmode = 0;
  ui_toast(ui, st < 0 ? "Não foi possível baixar o guia. Verifique a internet e tente de novo." : "A imagem do guia está inválida.", UI_TOAST_ERR);
}

static void shop_close(UI *ui) { ui->shop = 0; ui->dirty = 1; }

static void shop_reload(UI *ui) {
  shop_close(ui);
  if (!ui->reload_fn) return;
  ui_toast(ui, "Verificando seu plano no servidor...", UI_TOAST_INFO);
  ui_set_busy(ui, 1);
  ui->reload_fn(ui->auth_ud);
}

static void shop_key(UI *ui, int key) {
  ui->dirty = 1;
  if (ui->shop == 1) {
    int n = ui->plans.n;
    if (key == UI_LEFT && ui->shop_sel > 0) ui->shop_sel--;
    else if (key == UI_RIGHT && ui->shop_sel + 1 < n) ui->shop_sel++;
    else if (key == UI_OK) ui->shop = 2;
    else if (key == UI_SQUARE) shop_reload(ui);
    else if (key == UI_BACK || key == UI_OPTIONS || key == UI_TRIANGLE) shop_close(ui);
  } else if (ui->shop == 2) {
    if (key == UI_OK) ui->shop = 3;
    else if (key == UI_BACK || key == UI_TRIANGLE) ui->shop = 1;
    else if (key == UI_OPTIONS) shop_close(ui);
  } else {
    if (key == UI_OK || key == UI_SQUARE) shop_reload(ui);
    else if (key == UI_BACK || key == UI_TRIANGLE) ui->shop = 2;
    else if (key == UI_OPTIONS) shop_close(ui);
  }
}

void ui_key(UI *ui, int key) {
  ui->idle = 0;
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
  if (ui->gmode) { guide_key(ui, key); return; }                  /* guia aberto: so rolar ou voltar */
  if (ui->upd_help) {                                             /* passo a passo da atualizacao: qualquer botao fecha */
    if (key == UI_BACK || key == UI_OK || key == UI_OPTIONS || key == UI_TRIANGLE || key == UI_SQUARE) { ui->upd_help = 0; ui->dirty = 1; }
    return;
  }
  if (ui->shop) { shop_key(ui, key); return; }
  if (key == UI_OPTIONS && !ui->vid_mode) {
    if (ui->visitor) ui_toast(ui, "Entre ou cadastre-se para assinar um plano.", UI_TOAST_INFO);
    else if (ui->offline) ui_toast(ui, "Sem conexão com o servidor. Tente de novo quando a internet voltar.", UI_TOAST_INFO);
    else ui_open_shop(ui);
    return;
  }
  if (ui->vid_mode == 1 || (ui->vid_mode == 2 && (key == UI_TRIANGLE || key == UI_OPTIONS))) { vid_key(ui, key); return; }
  if (key == UI_TRIANGLE) {                                     /* sair da conta */
    if (ui->busy) { ui_toast(ui, "Aguarde o pedido atual terminar.", UI_TOAST_INFO); return; }
    vid_close(ui);
    if (ui->offline) { ui_toast(ui, "Sem conexão com o servidor. Feche e abra o app para tentar de novo.", UI_TOAST_INFO); return; }
    if (ui->visitor) {
      ui->detail = 0; ui->locked = ui->visitor = 0;
      ui_show_login(ui, "");
      return;
    }
    if (ui->logout_fn) ui->logout_fn(ui->auth_ud);
    ui->field[0][0] = 0;
    ui->detail = 0;
    ui->locked = 0;
    ui_show_login(ui, "Você saiu da conta.");
    return;
  }
  const CatGame *g = cur_game(ui);
  if (ui->detail && g) {                                          /* ---- detalhe: downloads (direita) e videos (esquerda) */
    int dl[CAT_MAX_BTNS], md[CAT_MAX_BTNS + 1], nd, nm;
    lists(g, dl, &nd, md, &nm);
    switch (key) {
      case UI_UP:    { int na = split_na(g), lo = (na && ui->dfocus >= na) ? na : 0;
                       if (ui->zone == 0 && ui->dfocus > lo) { ui->dfocus--; ui->dirty = 1; } } break;
      case UI_DOWN:  { int na = split_na(g), hi = (na && ui->dfocus < na) ? na : nd;
                       if (ui->zone == 0 && ui->dfocus + 1 < hi) { ui->dfocus++; ui->dirty = 1; } } break;
      case UI_LEFT:
        if (ui->zone == 0) {
          int na = split_na(g);
          if (na && ui->dfocus >= na) { int r = ui->dfocus - na; ui->dfocus = r < na ? r : na - 1; ui->dirty = 1; }   /* plugins -> apps */
          else if (nm > 0) { ui->zone = 1; ui->dirty = 1; }
        }
        else if (ui->mfocus > 0) { ui->mfocus--; ui->dirty = 1; }
        break;
      case UI_RIGHT:
        if (ui->zone == 1) {
          if (ui->mfocus + 1 < nm) ui->mfocus++;
          else if (nd > 0) ui->zone = 0;
          ui->dirty = 1;
        } else {
          int na = split_na(g);
          if (na && ui->dfocus < na && nd > na) { int r = ui->dfocus; ui->dfocus = na + (r < nd - na ? r : nd - na - 1); ui->dirty = 1; }   /* apps -> plugins */
        }
        break;
      case UI_BACK:  ui->detail = 0; ui->dirty = 1; rebuild_fbg(ui); break;
      case UI_OK:
        if (ui->zone == 0 && nd > 0) ui->focus = dl[ui->dfocus];
        else if (ui->zone == 1 && nm > 0) ui->focus = md[ui->mfocus];
        else break;
        trigger_install(ui);
        break;
      case UI_SQUARE: trigger_update(ui); break;
      default: break;
    }
    { int na = split_na(g);
      if (na && ui->dfocus >= na) {                                   /* modo dividido: coluna de plugins com rolagem propria */
        int r = ui->dfocus - na;
        if (r < ui->dscroll2) ui->dscroll2 = r;
        if (r >= ui->dscroll2 + DL_VIS) ui->dscroll2 = r - DL_VIS + 1;
      } else {
        if (ui->dfocus < ui->dscroll) ui->dscroll = ui->dfocus;
        if (ui->dfocus >= ui->dscroll + DL_VIS) ui->dscroll = ui->dfocus - DL_VIS + 1;
      } }
    if (ui->mfocus < ui->mscroll) ui->mscroll = ui->mfocus;
    { int vis = !strcmp(g->category, "duvidas") ? 4 : MD_VIS;
      if (ui->mfocus >= ui->mscroll + vis) ui->mscroll = ui->mfocus - vis + 1; }
    return;
  }
  switch (key) {                                                   /* ---- vitrine */
    case UI_LEFT:
      if (ui->sel > 0) { select_game(ui, ui->sel - 1); ui->slide = -(TH_W[1] + CAR_GAP); }
      break;
    case UI_RIGHT:
      if (ui->sel + 1 < ui->nfilt) { select_game(ui, ui->sel + 1); ui->slide = TH_W[1] + CAR_GAP; }
      break;
    case UI_L1:
    case UI_R1: {
      int n = ui->cat->ncat;
      ui->cat_idx = (ui->cat_idx + (key == UI_R1 ? 1 : n - 1)) % n;
      refilter(ui);
      select_game(ui, 0);
      break;
    }
    case UI_BACK:
      if (ui->cat_idx != 0 && !strncmp(ui->cat->cat[ui->cat_idx].id, "jkmod", 5)) {      /* dentro da pasta JK-MOD: volta para a pasta Plugins & Mods (card JK-MOD) */
        int back = 0;
        char from[64]; snprintf(from, sizeof from, "%s", ui->cat->cat[ui->cat_idx].label);   /* pasta de onde saimos */
        for (int k = 1; k < ui->cat->ncat; k++) if (!strcmp(ui->cat->cat[k].id, "plugins")) { ui->cat_idx = k; back = 1; break; }
        if (back) {
          int pos = 0;
          refilter(ui);
          for (int k = 0; k < ui->nfilt; k++) { const CatGame *q = &ui->cat->game[ui->filt[k]]; if (!strcmp(q->tag, "PASTA") && !strcmp(q->title, from)) { pos = k; break; } }
          select_game(ui, pos);
        }
      } else if (ui->cat_idx != 0 && !strcmp(ui->cat->cat[ui->cat_idx].id, "plugins")) {   /* dentro da pasta: volta para o card "Plugins & Mods" */
        int pos = 0;
        ui->cat_idx = 0;
        refilter(ui);
        for (int k = 0; k < ui->nfilt; k++) if (!strcmp(ui->cat->game[ui->filt[k]].category, "hub")) { pos = k; break; }
        select_game(ui, pos);
      } else if (ui->sel != 0) select_game(ui, 0);
      break;
    case UI_OK:
      if (g && !strcmp(g->tag, "PASTA")) {                         /* card-pasta: abre a categoria de mesmo nome (rotulo = titulo do card) */
        for (int k = 1; k < ui->cat->ncat; k++) if (!strcmp(ui->cat->cat[k].label, g->title)) {
          ui->cat_idx = k; refilter(ui); select_game(ui, 0); break;
        }
      } else if (g) {                                              /* abre o detalhe do jogo */
        int dl[CAT_MAX_BTNS], md[CAT_MAX_BTNS + 1], nd, nm;
        lists(g, dl, &nd, md, &nm);
        ui->detail = 1; ui->zone = nd > 0 ? 0 : 1;
        ui->dfocus = ui->dscroll = ui->dscroll2 = ui->mfocus = ui->mscroll = 0;
        ui->dirty = 1;
      }
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
  if (ui->auto_upd && ui->mode == MODE_CATALOG && ui->splash >= SPLASH_END && !ui->busy && !ui->offline) {
    ui->auto_upd = 0;
    if (update_available(ui)) trigger_update(ui);       /* atualizacao automatica: sem precisar apertar nada */
  }
  if (ui->slide) {                                    /* animacao de troca de capa: desacelera ate parar */
    ui->slide = ui->slide / 3;                         /* animacao curta (2 quadros): no PS4 cada quadro redesenha a tela toda */
    if (ui->slide > -40 && ui->slide < 40) ui->slide = 0;
    ui->dirty = 1;
  }
  if (ui->fbg_due && !ui->slide) { ui->fbg_due = 0; rebuild_fbg(ui); ui->dirty = 1; }
  if (ui->idle < 1000) ui->idle++;
  if (ui->idle > 20 && !ui->slide && !ui->fbg_due && ui->mode == MODE_CATALOG && !ui->detail && ui->bg) {   /* parado: prepara 1 fundo por quadro */
    for (int k = 0; k < ui->nfilt; k++) {
      int gi = ui->filt[k];
      if (ui->th[gi][0] && !ui->fbgc[gi]) {
        Image *c = img_new(W, H);
        if (c) { compose_fbg(ui, c, gi); ui->fbgc[gi] = c; }
        break;
      }
    }
  }
  if (ui->pi_on && ui->frame % 120 == 0) plan_refresh(ui);
  cover_poll(ui);
  guide_tick(ui);
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
  if (ui->mode == MODE_CATALOG && ui->visitor) {
    snprintf(st, sizeof(st), "VISITANTE");
  } else if (ui->mode == MODE_CATALOG) {
    snprintf(st, sizeof(st), "ONLINE  \xC2\xB7  ");
    size_t n = strlen(st);
    for (const char *q = ui->username; *q && n + 2 < sizeof(st); q++) st[n++] = (*q >= 'a' && *q <= 'z') ? *q - 32 : *q;
    st[n] = 0;
  } else {
    snprintf(st, sizeof(st), "%s", ui->mode == MODE_LOGIN ? "ACESSO RESTRITO" : ui->mode == MODE_QR ? "CADASTRO ENVIADO" : "CONECTANDO");
  }
  int w = TW(ui, 1, 20, st, 3);
  T(ui, 1, 20, W - MARGIN - w, 62, st, C_MUTED, 255, 3);
  gfx_circle(s, W - MARGIN - w - 22, 55, 7, (ui->mode == MODE_CATALOG && !ui->locked) ? C_ACCENT : C_FOCUS, 255);

  if (ui->mode == MODE_CATALOG && !ui->visitor) {          /* atalho da janela de planos: OPTIONS */
    const char *lab = (ui->locked || ui->plan_level >= 1) ? "Assinar plano" : "Meu plano";
    int kw = TW(ui, 1, 16, "OPTIONS", 1) + 22, lw = TW(ui, 0, 20, lab, 0);
    int cw = kw + lw + 40, cx = W - MARGIN - w - 22 - 7 - 44 - cw, cy = 33;
    gfx_rrect(s, cx, cy, cw, 44, 22, C_PANEL2, 255, 0);
    gfx_rrect(s, cx, cy, cw, 44, 22, C_ACCENT, 120, 2);
    gfx_rrect(s, cx + 8, cy + 8, kw, 28, 9, C_BG, 200, 0);
    text_draw_in(s, ui->fb, 16, cx + 8, kw, cy + 28, "OPTIONS", C_ACCENT, 255, 1, 1);
    T(ui, 0, 20, cx + 8 + kw + 12, cy + 30, lab, C_TEXT, 255, 0);
  }

  if (ui->mode == MODE_CATALOG && ui->plan[0]) {          /* plano e vencimento, logo abaixo do "ONLINE" */
    uint32_t pc = ui->plan_level == 2 ? C_ERR : ui->plan_level == 1 ? C_FOCUS : C_ACCENT;
    int pw = TW(ui, 1, 18, ui->plan, 2);
    T(ui, 1, 18, W - MARGIN - pw, 92, ui->plan, pc, 255, 2);
  }
}

/* "3. BAIXAR PATCH" -> "BAIXAR PATCH" (a numeracao do site nao faz sentido nas listas do app) */
static const char *strip_num(const char *l) {
  const char *p = l;
  while (*p >= '0' && *p <= '9') p++;
  if (p != l && p[0] == '.' && p[1] == ' ') return p + 2;
  return l;
}

static void draw_lock(Image *s, int cx, int cy, uint32_t col) {         /* cadeado ~28x34 centrado em cx,cy */
  gfx_rrect(s, cx - 10, cy - 17, 20, 22, 9, col, 255, 3);                /* argola */
  gfx_rrect(s, cx - 14, cy - 3, 28, 22, 5, col, 255, 0);                 /* corpo */
  gfx_circle(s, cx, cy + 7, 3, C_PANEL2, 255);                           /* furo */
}

static uint32_t g_btn_col = 0;
static int g_btn_compact = 0;                                    /* 1 = botao estreito (modo dividido): sem texto de info e letra menor */                                    /* 0 = verde padrao; senao cor do botao (plugins = amarelo) */
static void draw_button(UI *ui, const CatButton *b, int x, int y, int w, int focused) {
  Image *s = ui->s;
  int r = 20;
  if (ui->locked && !is_media(b)) {                                      /* bloqueado: cinza com cadeado */
    gfx_rrect(s, x, y, w, g_bh, r, C_PANEL2, 255, 0);
    gfx_rrect(s, x, y, w, g_bh, r, C_WHITE, 30, 2);
    char info[48];
    info_text(b, info, sizeof(info));
    if (g_btn_compact) info[0] = 0;
    int fs0 = g_btn_compact ? 23 : 27;
    int iw = info[0] ? TW(ui, 0, 20, info, 0) : 0;
    int avail = w - 40 - 70 - (iw ? iw + 24 : 0);
    char lab[128];
    text_fit(ui->fb, fs0, strip_num(b->label), avail, lab, sizeof(lab));
    T(ui, 1, fs0, x + 32, y + g_bh / 2 + 10, lab, C_MUTED, 255, 0);
    if (info[0]) T(ui, 0, 20, x + w - 70 - 12 - iw, y + g_bh / 2 + 7, info, C_MUTED, 170, 0);
    draw_lock(s, x + w - 38, y + g_bh / 2 - 1, C_MUTED);
    if (focused) gfx_rrect(s, x - 8, y - 8, w + 16, g_bh + 16, r + 8, C_FOCUS, 255, 4);
    return;
  }
  if (b->base) {
    gfx_rrect(s, x, y, w, g_bh, r, C_BASE, 255, 0);
    gfx_rrect(s, x, y, w, g_bh, r, C_BASE_EDGE, 255, 2);
  } else {
    gfx_rrect(s, x, y, w, g_bh, r, g_btn_col ? g_btn_col : C_ACCENT, 255, 0);
  }
  uint32_t fg = b->base ? C_WHITE : C_DARKTXT;
  char info[48];
  info_text(b, info, sizeof(info));
  if (g_btn_compact) info[0] = 0;
  int fs1 = g_btn_compact ? 23 : 27;
  int iw = info[0] ? TW(ui, 0, 20, info, 0) : 0;
  int arrow_w = 56;
  int avail = w - 40 - arrow_w - (iw ? iw + 24 : 0);
  char lab[128];
  text_fit(ui->fb, fs1, strip_num(b->label), avail, lab, sizeof(lab));
  T(ui, 1, fs1, x + 32, y + g_bh / 2 + 10, lab, fg, 255, 0);
  if (info[0]) T(ui, 0, 20, x + w - arrow_w - 12 - iw, y + g_bh / 2 + 7, info, fg, 190, 0);
  text_draw_in(s, ui->fb, 30, x + w - arrow_w - 4, arrow_w, y + g_bh / 2 + 11, "\xE2\x86\x93", fg, 255, 0, 1);
  if (!focused) gfx_rrect(s, x, y, w, g_bh, r, C_BG, 70, 0);
  else gfx_rrect(s, x - 8, y - 8, w + 16, g_bh + 16, r + 8, g_btn_col ? C_WHITE : C_FOCUS, 255, 4);
}


/* ---------------------------------------------------------------- vitrine (capas lado a lado) */
static void draw_placeholder(UI *ui, int x, int y, int w, int h, const char *title) {
  Image *s = ui->s;
  gfx_rrect(s, x, y, w, h, 18, C_PANEL2, 255, 0);
  gfx_rrect(s, x, y, w, h, 18, C_WHITE, 26, 2);
  char ini[8] = {0};
  ini[0] = title[0] ? title[0] : '?';
  text_draw_in(s, ui->fb, w / 2, x, w, y + h / 2 + w / 6, ini, C_MUTED, 255, 0, 1);
}

static void draw_tile(UI *ui, int idx, int d, int x, int y) {          /* d = distancia da capa em foco (0..3) */
  Image *s = ui->s;
  int gi = ui->filt[idx], w = TH_W[d], h = TH_H[d];
  const CatGame *g = &ui->cat->game[gi];
  gfx_rrect(s, x + 6, y + 14, w, h, 18, C_BLACK, 110, 0);                                  /* sombra */
  if (d == 0) gfx_rrect(s, x - 18, y - 18, w + 36, h + 36, 32, C_ACCENT, 28, 0);           /* brilho */
  if (ui->th[gi][d]) gfx_blit(s, ui->th[gi][d], x, y, 18, 255);
  else draw_placeholder(ui, x, y, w, h, g->title);
  if (d > 0) gfx_rrect(s, x, y, w, h, 18, C_BG, 80 + 40 * d, 0);                           /* escurece */
  else gfx_rrect(s, x - 7, y - 7, w + 14, h + 14, 24, C_ACCENT, 255, 5);
  if (d == 0 && g->tag[0]) {
    int tw = TW(ui, 1, 18, g->tag, 2);
    gfx_rrect(s, x + 16, y + 16, tw + 30, 36, 18, C_BG, 220, 0);
    text_draw_in(s, ui->fb, 18, x + 16, tw + 30, y + 41, g->tag, C_ACCENT, 255, 2, 1);
  }
}

static void draw_vitrine(UI *ui) {
  Image *s = ui->s;
  const CatGame *g = cur_game(ui);
  if (!g) {
    text_draw_in(s, ui->f, 30, 0, W, 560, "Nenhuma coleção encontrada nesta categoria.", C_MUTED, 255, 0, 1);
    return;
  }
  char buf[160];
  text_fit(ui->fb, 62, g->title, W - 2 * MARGIN, buf, sizeof(buf));
  text_draw_in(s, ui->fb, 62, 0, W, 212, buf, C_TEXT, 255, 0, 1);
  text_fit(ui->f, 26, g->kicker, W - 2 * MARGIN, buf, sizeof(buf));
  text_draw_in(s, ui->f, 26, 0, W, 256, buf, C_MUTED, 255, 0, 1);
  char cnt[120];
  snprintf(cnt, sizeof(cnt), "%s   ·   %d de %d", ui->cat->cat[ui->cat_idx].label, ui->sel + 1, ui->nfilt);
  text_draw_in(s, ui->fb, 22, 0, W, 296, cnt, C_ACCENT, 255, 1, 1);

  int y0 = CAR_Y, xc = (W - TH_W[0]) / 2;
  int xr = xc + TH_W[0] + CAR_GAP, xl = xc - CAR_GAP;                /* proximas posicoes livres (direita / esquerda) */
  int xs[7], ys[7], ds[7], n = 0;
  for (int d = 1; d <= 3; d++) {
    if (ui->sel + d < ui->nfilt) { xs[n] = xr; ys[n] = y0 + (TH_H[0] - TH_H[d]) / 2; ds[n] = d; n++; }
    xr += TH_W[d] + CAR_GAP;
    if (ui->sel - d >= 0) { xs[n] = xl - TH_W[d]; ys[n] = y0 + (TH_H[0] - TH_H[d]) / 2; ds[n] = -d; n++; }
    xl -= TH_W[d] + CAR_GAP;
  }
  for (int i = n - 1; i >= 0; i--) {                                  /* de fora para dentro */
    int d = ds[i] < 0 ? -ds[i] : ds[i];
    draw_tile(ui, ui->sel + ds[i], d, xs[i] + ui->slide, ys[i]);
  }
  draw_tile(ui, ui->sel, 0, xc + ui->slide, y0);
}

/* ---------------------------------------------------------------- detalhe do jogo */
static void draw_media_card(UI *ui, int x, int y, int w, int h, const char *label, int is_image, int focused) {
  Image *s = ui->s;
  int th = h - 56;
  gfx_rrect(s, x, y, w, h, 16, C_PANEL2, 255, 0);
  gfx_rrect(s, x, y, w, th, 16, C_BG, 150, 0);
  gfx_rrect(s, x, y, w, h, 16, C_WHITE, 36, 2);
  int cx = x + w / 2, cy = y + th / 2;
  gfx_circle(s, cx, cy, 30, C_ACCENT, focused ? 255 : 150);
  if (!is_image) text_draw_in(s, ui->fb, 30, cx - 30, 60, cy + 10, "▶", C_DARKTXT, 255, 0, 1);
  else {
    gfx_rrect(s, cx - 18, cy - 13, 36, 26, 5, C_DARKTXT, 255, 3);
    gfx_circle(s, cx + 7, cy - 4, 4, C_DARKTXT, 255);
  }
  char lb[128];
  text_fit(ui->fb, 22, strip_num(label), w - 32, lb, sizeof(lb));
  T(ui, 1, 22, x + 16, y + h - 22, lb, C_TEXT, 255, 0);
  if (focused) gfx_rrect(s, x - 7, y - 7, w + 14, h + 14, 22, C_FOCUS, 255, 4);
}

/* Descricao em topicos: os itens separados por " \xE2\x80\xA2 " (bolinha) viram uma linha cada, com marcador; o primeiro
 * (a atualizacao) fica em destaque. Sem esse separador, desenha como texto corrido (catalogos antigos). Retorna as linhas usadas. */
static int draw_desc(UI *ui, int x, int y, int w, int lh, int max_lines, const char *desc) {
  Image *s = ui->s;
  static const char SEP[] = " \xE2\x80\xA2 ";
  if (!strstr(desc, SEP)) return text_wrap(s, ui->f, 23, x, y, w, lh, max_lines, desc, C_MUTED, 255);
  int used = 0, first = 1;
  const char *p = desc;
  while (*p && used < max_lines) {
    const char *e = strstr(p, SEP);
    size_t n = e ? (size_t)(e - p) : strlen(p);
    char item[512];
    if (n >= sizeof(item)) n = sizeof(item) - 1;
    memcpy(item, p, n);
    item[n] = 0;
    if (item[0]) {
      int ly = y + used * lh;
      gfx_circle(s, x + 7, ly - 8, 5, first ? C_ACCENT : C_MUTED, first ? 255 : 170);
      int ln = text_wrap(s, first ? ui->fb : ui->f, 23, x + 28, ly, w - 28, lh, max_lines - used, item,
                         first ? C_TEXT : C_MUTED, 255);
      used += ln > 0 ? ln : 1;
    }
    first = 0;
    if (!e) break;
    p = e + (sizeof(SEP) - 1);
  }
  return used;
}

/* ---- tela "Duvidas & Respostas": dois paineis lado a lado, lidos da descricao do card.
 * desc = "<painel 1> ## <painel 2>"; cada painel = "Titulo | Aviso | 1 passo | 2 passo | ... | nota" (separador " \xE2\x80\xA2 ").
 * Item que comeca com digito = passo; o primeiro item e o titulo, o segundo e a faixa de aviso; os demais sao notas. */
static Image *faq_logo(UI *ui, const char *needle, Image **cache) {
  if (*cache) return *cache;
  for (int i = 0; i < (ui->cat ? ui->cat->ngame : 0); i++) {
    const CatGame *gg = &ui->cat->game[i];
    if (strstr(gg->title, needle) && ui->th[i][3]) { *cache = img_scale(ui->th[i][3], 84, 142); break; }
  }
  return *cache;
}

static void faq_panel(UI *ui, int px, int py, int pw, int ph, const char *sec, uint32_t accent, Image *logo) {
  Image *s = ui->s;
  static const char SEP[] = " \xE2\x80\xA2 ";
  char items[10][300]; int n = 0; const char *p = sec;
  while (*p && n < 10) {
    const char *e = strstr(p, SEP); size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l >= sizeof(items[0])) l = sizeof(items[0]) - 1;
    memcpy(items[n], p, l); items[n][l] = 0; n++;
    if (!e) break; p = e + (sizeof(SEP) - 1);
  }
  gfx_rrect(s, px, py, pw, ph, 26, C_PANEL, 235, 0);
  gfx_rrect(s, px, py, pw, ph, 26, C_WHITE, 26, 2);
  gfx_rrect(s, px, py, pw, 10, 5, accent, 255, 0);
  if (logo) {
    int lx = px + pw - 44 - logo->w, ly = py + 26;
    gfx_rrect(s, lx - 3, ly - 3, logo->w + 6, logo->h + 6, 12, accent, 255, 3);
    gfx_blit(s, logo, lx, ly, 10, 255);
  }
  int y = py + 74;
  if (n > 0) T(ui, 1, 36, px + 44, y, items[0], C_TEXT, 255, 1);
  if (n > 1) {
    int bw = TW(ui, 1, 20, items[1], 1) + 36;
    gfx_rrect(s, px + 44, y + 18, bw, 40, 20, C_PANEL2, 255, 0);
    text_draw_in(s, ui->fb, 20, px + 44, bw, y + 46, items[1], C_FOCUS, 255, 1, 1);
  }
  y += 112;
  int step = 0;
  for (int i = 2; i < n; i++) {
    unsigned char c0 = (unsigned char)items[i][0];
    if (c0 < '1' || c0 > '9') continue;
    step++;
    gfx_circle(s, px + 44 + 21, y - 8, 21, C_BASE, 255);
    gfx_circle(s, px + 44 + 21, y - 8, 21, C_ACCENT, 120);
    char num[4]; snprintf(num, sizeof(num), "%d", step);
    text_draw_in(s, ui->fb, 24, px + 44, 42, y, num, C_TEXT, 255, 0, 1);
    const char *sp = strchr(items[i], ' ');
    const char *body = sp ? sp + 1 : items[i];
    int ln = text_wrap(s, ui->f, 24, px + 44 + 62, y, pw - 44 - 62 - 36, 31, 3, body, C_TEXT, 255);
    if (ln < 1) ln = 1;
    y += ln * 31 + 20;
  }
  int ny = py + ph - 26;
  for (int i = n - 1; i >= 2; i--) {
    unsigned char c0 = (unsigned char)items[i][0];
    if (c0 >= '1' && c0 <= '9') continue;
    int bh = 2 * 28 + 30;
    gfx_rrect(s, px + 30, ny - bh, pw - 60, bh, 16, C_FOCUS, 30, 0);
    gfx_rrect(s, px + 30, ny - bh, pw - 60, bh, 16, C_FOCUS, 210, 2);
    text_wrap(s, ui->f, 21, px + 54, ny - bh + 38, pw - 108, 28, 2, items[i], C_TEXT, 255);
    ny -= bh + 12;
  }
}

static void draw_faq(UI *ui, const CatGame *g) {
  Image *s = ui->s;
  const char *m = strstr(g->desc, " ## ");
  char a[600];
  size_t la = m ? (size_t)(m - g->desc) : strlen(g->desc);
  if (la >= sizeof(a)) la = sizeof(a) - 1;
  memcpy(a, g->desc, la); a[la] = 0;
  int top = 130, hh = 600, gap = 28, pw = (1920 - 2 * 96 - gap) / 2;
  static Image *lgM, *lgB;
  faq_panel(ui, 96, top, pw, hh, a, C_ACCENT, faq_logo(ui, "Mega Patch", &lgM));
  faq_panel(ui, 96 + pw + gap, top, pw, hh, m ? m + 4 : "", C_FOCUS, faq_logo(ui, "Brasfootball", &lgB));

  /* faixa de videos e tutoriais, cobrindo os dois paineis */
  int dl[CAT_MAX_BTNS], md[CAT_MAX_BTNS + 1], nd, nm;
  lists(g, dl, &nd, md, &nm);
  int bx = 96, bw = 1920 - 2 * 96, by = top + hh + 18, bh = 190;
  gfx_rrect(s, bx, by, bw, bh, 26, C_PANEL, 235, 0);
  gfx_rrect(s, bx, by, bw, bh, 26, RGB(0x38, 0xbd, 0xf8), 90, 2);
  T(ui, 1, 20, bx + 40, by + 38, "VÍDEOS E TUTORIAIS", RGB(0x38, 0xbd, 0xf8), 255, 3);
  { char cap[256];
    text_fit(ui->f, 22, "Qualquer dúvida sobre instalações manuais ou manuseio dos arquivos, siga os vídeos e tutoriais abaixo.", bw - 370 - 130, cap, sizeof(cap));
    T(ui, 0, 22, bx + 370, by + 38, cap, C_MUTED, 255, 0); }
  if (nm > 0) {
    char pg[32]; snprintf(pg, sizeof(pg), "%d / %d", ui->mfocus + 1, nm);
    int pw2 = TW(ui, 0, 20, pg, 0);
    T(ui, 0, 20, bx + bw - 40 - pw2, by + 38, pg, C_MUTED, 255, 0);
    int vis = 4, cw2 = (bw - 80 - (vis - 1) * 20) / vis;
    for (int i = 0; i < vis; i++) {
      int mi = ui->mscroll + i;
      if (mi >= nm) break;
      int bi = md[mi];
      const char *label = g->btn[bi].label; int is_img = !strcmp(g->btn[bi].action, "image");
      draw_media_card(ui, bx + 40 + i * (cw2 + 20), by + 62, cw2, 112, label, is_img, ui->zone == 1 && mi == ui->mfocus);
    }
    if (ui->mscroll > 0) T(ui, 1, 24, bx + 8, by + 120, "◀", C_ACCENT, 255, 0);
    if (ui->mscroll + vis < nm) T(ui, 1, 24, bx + bw - 30, by + 120, "▶", C_ACCENT, 255, 0);
  }
}

static void draw_dl_panel(UI *ui, const CatGame *g, const int *dl, int lo, int hi, int px, int pw, const char *title, uint32_t col, int scroll, int notes) {
  Image *s = ui->s;
  int n = hi - lo;
  gfx_rrect(s, px, DT_PY, pw, DT_PH, 26, C_PANEL, 235, 0);
  gfx_rrect(s, px, DT_PY, pw, DT_PH, 26, C_WHITE, 26, 2);
  if (col) gfx_rrect(s, px + 30, DT_PY + 14, pw - 60, 6, 3, col, 255, 0);       /* faixa colorida do topo (modo dividido) */
  T(ui, 1, 26, px + 44, DT_PY + 58, title, col ? col : C_TEXT, 255, 3);
  char ct[48];
  int fr = ui->dfocus - lo;
  if (n && fr >= 0 && fr < n) snprintf(ct, sizeof(ct), "%d / %d", fr + 1, n); else snprintf(ct, sizeof(ct), "%d", n);
  int cw3 = TW(ui, 0, 22, ct, 0);
  T(ui, 0, 22, px + pw - 44 - cw3, DT_PY + 58, ct, C_MUTED, 255, 0);
  gfx_fill(s, px + 30, DT_PY + 82, pw - 60, 1, C_WHITE, 26);
  if (n == 0) {
    text_draw_in(s, ui->f, 26, px, pw, DT_PY + 200, "Nenhum item ainda.", C_MUTED, 255, 0, 1);
  } else {
    g_bh = BTN_H;
    g_btn_col = col; g_btn_compact = notes >= 0 && ui->dfocus >= 0 && split_mode(g);
    int by = DT_PY + 104, bx = px + 44, bw = pw - 88 - 18;
    for (int i = 0; i < DL_VIS; i++) {
      int di = scroll + i;
      if (di >= n) break;
      draw_button(ui, &g->btn[dl[lo + di]], bx, by + i * (g_bh + BTN_GAP), bw, ui->zone == 0 && lo + di == ui->dfocus);
    }
    g_btn_col = 0; g_btn_compact = 0;
    if (n > DL_VIS) {                                               /* barra de rolagem */
      int tr = DL_VIS * (g_bh + BTN_GAP) - BTN_GAP, sx = px + pw - 38;
      gfx_rrect(s, sx, by, 8, tr, 4, C_PANEL2, 255, 0);
      int bh = tr * DL_VIS / n; if (bh < 50) bh = 50;
      int bt = by + (tr - bh) * scroll / (n - DL_VIS);
      gfx_rrect(s, sx, bt, 8, bh, 4, col ? col : C_ACCENT, 255, 0);
    }
  }
  if (!notes) return;
  { char dtxt2[1200]; const char *note = strstr(g->desc, " !! "); (void)dtxt2;
    if (note) note += 4;
    if (ui->locked) {
      text_wrap(s, ui->fb, 20, px + 44, DT_PY + DT_PH - 118, pw - 88, 28, 2,
                ui->offline ? "Servidor fora do ar: mostrando a última versão salva. Os downloads voltam quando ele voltar." : "Downloads bloqueados: ative seu plano para baixar. Vídeos, tutoriais e descrições continuam liberados.", C_FOCUS, 255);
    } else if (note && note[0]) {
      gfx_rrect(s, px + 30, DT_PY + DT_PH - 68, pw - 60, 58, 14, C_FOCUS, 36, 0);
      gfx_rrect(s, px + 30, DT_PY + DT_PH - 68, pw - 60, 58, 14, C_FOCUS, 200, 2);
      text_wrap(s, ui->fb, 18, px + 46, DT_PY + DT_PH - 44, pw - 92, 26, 2, note, C_FOCUS, 255);
    } else if (!split_mode(g))
    text_wrap(s, ui->f, 18, px + 44, DT_PY + DT_PH - 52, pw - 88, 26, 2,
              "Antes de instalar: confira a versão de cada arquivo. O jogo base precisa estar instalado antes do update. "
              "A instalação é feita pelo PS4; acompanhe em Notificações.", C_MUTED, 200);
  }
}

static void draw_detail(UI *ui) {
  Image *s = ui->s;
  const CatGame *g = cur_game(ui);
  if (!g) return;
  if (!strcmp(g->category, "duvidas")) { draw_faq(ui, g); return; }
  int gi = ui->filt[ui->sel];
  int dl[CAT_MAX_BTNS], md[CAT_MAX_BTNS + 1], nd, nm;
  lists(g, dl, &nd, md, &nm);
  int na = split_na(g), split = split_mode(g);
  int LW = split ? 560 : DT_W;                                    /* largura da coluna de info (modo dividido: 3 colunas) */

  /* --- coluna da esquerda: capa + titulo, descricao, videos */
  int cx = DT_X, cy = 140, cw = TH_W[2], chh = TH_H[2];
  gfx_rrect(s, cx + 5, cy + 12, cw, chh, 18, C_BLACK, 110, 0);
  if (ui->th[gi][2]) gfx_blit(s, ui->th[gi][2], cx, cy, 18, 255);
  else draw_placeholder(ui, cx, cy, cw, chh, g->title);
  gfx_rrect(s, cx - 4, cy - 4, cw + 8, chh + 8, 21, C_ACCENT, 255, 4);

  int tx = cx + cw + 36, tw = DT_X + LW - tx;
  int tl = text_wrap(s, ui->fb, 40, tx, cy + 46, tw, 48, 3, g->title, C_TEXT, 255);
  if (tl < 1) tl = 1;
  int ky = cy + 46 + (tl - 1) * 48 + 44;
  char kb[160];
  text_fit(ui->f, 21, g->kicker, tw, kb, sizeof(kb));
  T(ui, 0, 21, tx, ky, kb, C_ACCENT, 255, 1);
  char info[96];
  snprintf(info, sizeof(info), "%d download%s  ·  %d vídeo%s/guia%s", nd, nd == 1 ? "" : "s",
           nm, nm == 1 ? "" : "s", nm == 1 ? "" : "s");
  T(ui, 0, 20, tx, ky + 40, info, C_MUTED, 255, 0);
  if (g->tag[0]) {
    int bw = TW(ui, 1, 18, g->tag, 2) + 30;
    gfx_rrect(s, tx, ky + 68, bw, 36, 18, C_PANEL2, 255, 0);
    text_draw_in(s, ui->fb, 18, tx, bw, ky + 93, g->tag, C_ACCENT, 255, 2, 1);
  }

  int dy = cy + chh + 40;
  /* aviso amarelo no rodape: o que vier depois de " !! " na descricao nao entra na lista de topicos */
  char dtxt[1200]; const char *note = strstr(g->desc, " !! ");
  if (note) { size_t nn = (size_t)(note - g->desc); if (nn >= sizeof(dtxt)) nn = sizeof(dtxt) - 1; memcpy(dtxt, g->desc, nn); dtxt[nn] = 0; note += 4; }
  draw_desc(ui, DT_X, dy, LW, 31, 7, note ? dtxt : g->desc);

  /* videos e guias */
  int my = 742;
  T(ui, 1, 20, DT_X, my, "VÍDEOS E TUTORIAIS", RGB(0x38, 0xbd, 0xf8), 255, 3);
  if (nm == 0) {
    T(ui, 0, 22, DT_X, my + 52, "Nenhum vídeo ou guia para este jogo.", C_MUTED, 200, 0);
  } else {
    char pg[32];
    snprintf(pg, sizeof(pg), "%d / %d", ui->mfocus + 1, nm);
    int pw = TW(ui, 0, 20, pg, 0);
    T(ui, 0, 20, DT_X + LW - pw, my, pg, C_MUTED, 255, 0);
    int cw2 = (LW - 24) / 2;
    for (int i = 0; i < MD_VIS; i++) {
      int mi = ui->mscroll + i;
      if (mi >= nm) break;
      int bi = md[mi];
      const char *label; int is_img = 0;
      if (bi < g->nbtn) { label = g->btn[bi].label; is_img = !strcmp(g->btn[bi].action, "image"); }
      else label = "Vídeo do patch";
      draw_media_card(ui, DT_X + i * (cw2 + 24), my + 24, cw2, 132, label, is_img, ui->zone == 1 && mi == ui->mfocus);
    }
    if (ui->mscroll > 0) T(ui, 1, 24, DT_X - 34, my + 90, "◀", C_ACCENT, 255, 0);
    if (ui->mscroll + MD_VIS < nm) T(ui, 1, 24, DT_X + LW + 8, my + 90, "▶", C_ACCENT, 255, 0);
  }

  /* --- painel(is) da direita: downloads (modo dividido: APPs verde + PLUGINS amarelo) */
  if (split) {
    int pw2 = 550, ax = 698, bx2 = ax + pw2 + 24;
    draw_dl_panel(ui, g, dl, 0, na, ax, pw2, "APPS", C_ACCENT, ui->dscroll, 0);
    draw_dl_panel(ui, g, dl, na, nd, bx2, pw2, "PLUGINS", C_FOCUS, ui->dscroll2, 1);
  } else
    draw_dl_panel(ui, g, dl, 0, nd, DT_PX, DT_PW, "DOWNLOADS", 0, ui->dscroll, 1);
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
    int x = (W - w) / 2, y = 916;
    gfx_rrect(s, x, y, w, h, h / 2, C_PANEL2, 250, 0);
    gfx_rrect(s, x, y, w, h, h / 2, edge, 255, 3);
    if (ui->busy) draw_spinner(ui, x + pad + 6, y + h / 2);
    T(ui, 0, 26, x + pad + extra, y + h / 2 + 9, buf, C_TEXT, 255, 0);
    return;
  }
  if (update_available(ui)) {
    char buf[200];
    if (ui->upd_ready)
      snprintf(buf, sizeof(buf), "Nova versão %s baixada.  Aperte  \xE2\x96\xA1  para ver como instalar.", ui->cat->app.version);
    else
      snprintf(buf, sizeof(buf), "Atualização disponível: JK Patchs %s (você tem %s).  Aperte  \xE2\x96\xA1  para baixar.",
               ui->cat->app.version, APP_VERSION);
    int tw = TW(ui, 1, 26, buf, 0);
    int w = tw + 68, h = 62, x = (W - w) / 2, y = 916;
    gfx_rrect(s, x, y, w, h, h / 2, C_PANEL2, 250, 0);
    gfx_rrect(s, x, y, w, h, h / 2, C_FOCUS, 255, 3);
    T(ui, 1, 26, x + 34, y + h / 2 + 9, buf, C_TEXT, 255, 0);
    return;
  }
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
    if (ui->preview_fn) x = hint(ui, x, "\xE2\x97\x80", "no teclado: Ver a loja");
  } else if (ui->mode == MODE_QR) {
    x = hint(ui, x, "O", "Voltar ao login");
  } else if (ui->mode == MODE_CATALOG && ui->shop) {
    if (ui->shop == 1) {
      x = hint(ui, x, "\xE2\x97\x80 \xE2\x96\xB6", "Escolher");
      x = hint(ui, x, "X", "Assinar");
      x = hint(ui, x, "\xE2\x96\xA1", "Atualizar meu plano");
      x = hint(ui, x, "O", ui->locked ? "Só olhar a loja" : "Fechar");
    } else if (ui->shop == 2) {
      x = hint(ui, x, "X", "Já paguei");
      x = hint(ui, x, "O", "Voltar");
    } else {
      x = hint(ui, x, "X", "Já enviei: atualizar plano");
      x = hint(ui, x, "O", "Voltar");
    }
  } else if (ui->mode == MODE_CATALOG && ui->upd_help) {
    x = hint(ui, x, "O", "Voltar");
  } else if (ui->mode == MODE_CATALOG && ui->detail) {
    x = hint(ui, x, "\xE2\x97\x80 \xE2\x96\xB2 \xE2\x96\xBC \xE2\x96\xB6", "Mover");
    x = hint(ui, x, "X", ui->zone == 0 ? (ui->locked ? "Bloqueado" : "Baixar") : "Assistir / ver");
    x = hint(ui, x, "O", "Voltar");
    if (update_available(ui)) x = hint(ui, x, "\xE2\x96\xA1", "Atualizar app");
  } else if (ui->mode == MODE_CATALOG) {
    x = hint(ui, x, "\xE2\x97\x80 \xE2\x96\xB6", "Coleções");
    x = hint(ui, x, "X", "Detalhes");
    x = hint(ui, x, "L1  R1", "Categorias");
    x = hint(ui, x, "O", "Início");
    x = hint(ui, x, "\xE2\x96\xB3", ui->visitor ? "Voltar ao login" : "Sair");
    if (update_available(ui)) x = hint(ui, x, "\xE2\x96\xA1", "Atualizar app");
  }
  char v[48];
  snprintf(v, sizeof(v), "JK Patchs  v%s", APP_VERSION);
  int w = TW(ui, 0, 20, v, 1);
  T(ui, 0, 20, W - MARGIN - w, 1036, v, C_MUTED, 150, 1);
}

/* ---- telas de login / carregando */
#define QR_X   480
#define QR_W   960
#define QR_Y   130
#define QR_H   810
#define LG_X   864
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
  /* lado esquerdo: marca + aviso do cadastro com o QR do WhatsApp */
  {
    int bx = MARGIN, bw = LG_X - 70 - MARGIN, bc = bx + bw / 2;
    if (ui->logo_full) gfx_blit(s, ui->logo_full, bc - ui->logo_full->w / 2, 130, 0, 255);
    text_draw_in(s, ui->fb, 58, bx, bw, 560, "JK PATCHS", C_TEXT, 255, 5, 1);
    text_draw_in(s, ui->f, 26, bx, bw, 604, "Os melhores patches de futebol para o seu PS4.", C_MUTED, 255, 0, 1);
    if (ui->preview_fn) {                                          /* botao piscante: ver a loja antes de cadastrar */
      int by = 628, bh = 64, on = (ui->frame / 30) % 2 == 0, sel = ui->kr < 0;
      gfx_rrect(s, bx, by, bw, bh, 32, on ? C_ACCENT : C_BASE, 255, 0);
      gfx_rrect(s, bx, by, bw, bh, 32, C_BASE_EDGE, 255, 3);
      text_draw_in(s, ui->fb, 26, bx, bw, by + bh / 2 + 9, "Ver a Loja antes de fazer o Cadastro", on ? C_DARKTXT : C_WHITE, 255, 0, 1);
      if (sel) gfx_rrect(s, bx - 6, by - 6, bw + 12, bh + 12, 38, C_FOCUS, 255, 4);
    }
    int qy = 722;                                                  /* comunicado: como entrar (o QR do WhatsApp aparece depois do cadastro) */
    gfx_rrect(s, bx, qy, bw, 196, 18, C_PANEL, 220, 0);
    gfx_rrect(s, bx, qy, bw, 196, 18, C_WHITE, 26, 2);
    T(ui, 1, 20, bx + 28, qy + 38, "COMO ENTRAR", C_ACCENT, 255, 3);
    static const char *step[3] = {
      "Toque em CADASTRAR e crie seu usuário e senha.",
      "Aparece um QR Code: fale com a gente no WhatsApp.",
      "Quando liberarmos sua conta, é só tocar em ENTRAR."};
    for (int i = 0; i < 3; i++) {
      int sy = qy + 76 + i * 38;
      gfx_circle(s, bx + 40, sy - 8, 14, C_BASE, 255);
      char n[2] = {(char)('1' + i), 0};
      text_draw_in(s, ui->fb, 18, bx + 26, 28, sy - 1, n, C_WHITE, 255, 0, 1);
      char buf[160];
      text_fit(ui->f, 22, step[i], bw - 90, buf, sizeof(buf));
      T(ui, 0, 22, bx + 68, sy, buf, C_TEXT, 255, 0);
    }
  }
  T(ui, 1, 22, px, LG_Y + 62, "ÁREA DE MEMBROS", C_ACCENT, 255, 4);
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
  gfx_rrect(s, QR_X, QR_Y, QR_W, QR_H, 28, C_PANEL, 245, 0);
  gfx_rrect(s, QR_X, QR_Y, QR_W, QR_H, 28, C_WHITE, 26, 2);
  text_draw_in(s, ui->fb, 44, QR_X, QR_W, QR_Y + 72, "Cadastro enviado!", C_ACCENT, 255, 0, 1);
  char l[160];
  snprintf(l, sizeof(l), "Usuário: %s", ui->qr_user);
  text_draw_in(s, ui->fb, 30, QR_X, QR_W, QR_Y + 122, l, C_TEXT, 255, 0, 1);
  text_draw_in(s, ui->f, 26, QR_X, QR_W, QR_Y + 168, "Sua conta está aguardando ativação.", C_MUTED, 255, 0, 1);
  int q = 450, qx = QR_X + (QR_W - q) / 2, qy = QR_Y + 192;     /* o PNG ja tem 450x450 (10 px por modulo): sem redimensionar */
  gfx_rrect(s, qx - 8, qy - 8, q + 16, q + 16, 18, RGB(255, 255, 255), 255, 0);
  if (ui->qr) {
    gfx_blit(s, ui->qr, qx, qy, 0, 255);
  } else {
    text_draw_in(s, ui->f, 24, qx, q, qy + q / 2, "(QR Code indisponível)", C_DARKTXT, 255, 0, 1);
  }
  text_draw_in(s, ui->fb, 28, QR_X, QR_W, qy + q + 58, "Escaneie com o celular e fale com o admin no WhatsApp", C_TEXT, 255, 0, 1);
  text_draw_in(s, ui->f, 24, QR_X, QR_W, qy + q + 96, "Quando ele ativar sua conta (30 dias), volte e toque em ENTRAR.", C_MUTED, 255, 0, 1);
}

/* Desenha um QR Code gerado na hora (fundo branco com margem). box = lado maximo em pixels. */
static void draw_qr_text(UI *ui, const char *txt, int ecc, int cxm, int y, int box) {
  static uint8_t m[QR_MAX_SIZE * QR_MAX_SIZE];
  Image *s = ui->s;
  int n = qr_encode(txt, ecc, m);
  if (!n) { text_draw_in(s, ui->f, 24, cxm - box / 2, box, y + box / 2, "(QR Code indisponível)", C_TEXT, 255, 0, 1); return; }
  int sc = box / (n + 8);
  if (sc < 2) sc = 2;
  int total = sc * (n + 8), x0 = cxm - total / 2;
  gfx_rrect(s, x0, y, total, total, 16, RGB(255, 255, 255), 255, 0);
  for (int j = 0; j < n; j++) {
    int i = 0;
    while (i < n) {
      if (!m[j * n + i]) { i++; continue; }
      int k = i;
      while (k < n && m[j * n + k]) k++;
      gfx_fill(s, x0 + (i + 4) * sc, y + (j + 4) * sc, (k - i) * sc, sc, RGB(0, 0, 0), 255);
      i = k;
    }
  }
}

static void shop_button(UI *ui, int x, int y, int w, int h, const char *label) {
  Image *s = ui->s;
  gfx_rrect(s, x, y, w, h, 18, C_BASE, 255, 0);
  gfx_rrect(s, x, y, w, h, 18, C_BASE_EDGE, 255, 2);
  gfx_rrect(s, x - 5, y - 5, w + 10, h + 10, 22, C_FOCUS, 255, 4);
  text_draw_in(s, ui->fb, 30, x, w, y + h / 2 + 11, label, C_WHITE, 255, 0, 1);
}

static void draw_shop(UI *ui) {
  Image *s = ui->s;
  gfx_fill(s, 0, 0, W, H, C_BLACK, 205);
  const AuthPlans *pl = &ui->plans;
  if (pl->n <= 0) return;
  const AuthPlan *sel = &pl->plan[ui->shop_sel < pl->n ? ui->shop_sel : 0];
  char money[32], t[260];

  if (ui->shop == 1) {
    int cw = 1500, ch = 780, cx = (W - cw) / 2, cy = 110;
    gfx_rrect(s, cx, cy, cw, ch, 28, C_PANEL, 250, 0);
    gfx_rrect(s, cx, cy, cw, ch, 28, C_WHITE, 26, 2);
    text_draw_in(s, ui->fb, 46, cx, cw, cy + 84, "Escolha seu plano", C_ACCENT, 255, 0, 1);
    const char *msg;
    if (ui->locked) msg = "Seu acesso está bloqueado. Assine para voltar a baixar.";
    else if (ui->plan_level >= 1) msg = ui->plan[0] ? ui->plan : "Renove seu plano";
    else msg = "Seu plano está ativo. Se renovar agora, os dias são somados.";
    text_fit(ui->f, 28, msg, cw - 120, t, sizeof(t));
    text_draw_in(s, ui->f, 28, cx, cw, cy + 140, t, ui->locked ? C_ERR : (ui->plan_level >= 1 ? C_FOCUS : C_MUTED), 255, 0, 1);

    int n = pl->n, gap = 40, bw = (cw - 100 - gap * (n - 1)) / n, bh = 470, by = cy + 190;
    double mensal = 0;
    for (int i = 0; i < n; i++) if (pl->plan[i].days <= 31) { mensal = atof(pl->plan[i].price); break; }
    for (int i = 0; i < n; i++) {
      const AuthPlan *p = &pl->plan[i];
      int bx = cx + 50 + i * (bw + gap), on = i == ui->shop_sel;
      gfx_rrect(s, bx, by, bw, bh, 24, on ? C_PANEL2 : C_BG, 255, 0);
      gfx_rrect(s, bx, by, bw, bh, 24, C_WHITE, on ? 40 : 22, 2);
      if (on) gfx_rrect(s, bx - 5, by - 5, bw + 10, bh + 10, 28, C_FOCUS, 255, 4);
      text_draw_in(s, ui->fb, 40, bx, bw, by + 80, p->name, C_TEXT, 255, 0, 1);
      auth_money(p->price, money, sizeof(money));
      text_draw_in(s, ui->fb, 64, bx, bw, by + 190, money, C_ACCENT, 255, 0, 1);
      if (p->days == 30) snprintf(t, sizeof(t), "30 dias");
      else if (p->days % 30 == 0 || p->days == 365) snprintf(t, sizeof(t), "%d dias", p->days);
      else snprintf(t, sizeof(t), "%d dias", p->days);
      text_draw_in(s, ui->f, 30, bx, bw, by + 250, t, C_TEXT, 255, 0, 1);
      double per = atof(p->price) / (p->days / 30.0);
      char pm[32], pmm[40];
      snprintf(pm, sizeof(pm), "%.2f", per);
      auth_money(pm, pmm, sizeof(pmm));
      snprintf(t, sizeof(t), "%s por mês", pmm);
      text_draw_in(s, ui->f, 26, bx, bw, by + 300, t, C_MUTED, 255, 0, 1);
      if (mensal > 0 && p->days > 31) {
        int save = (int)((1.0 - per / mensal) * 100.0 + 0.5);
        if (save > 0) {
          snprintf(t, sizeof(t), "ECONOMIZE %d%%", save);
          int tw = text_width(ui->fb, 24, t, 1) + 40;
          gfx_rrect(s, bx + (bw - tw) / 2, by + 340, tw, 46, 23, C_FOCUS, 255, 0);
          text_draw_in(s, ui->fb, 24, bx, bw, by + 372, t, C_DARKTXT, 255, 1, 1);
        }
      }
      if (on) text_draw_in(s, ui->fb, 26, bx, bw, by + bh - 36, "X  ASSINAR", C_FOCUS, 255, 1, 1);
    }
    text_draw_in(s, ui->f, 24, cx, cw, cy + ch - 30, "Pague pelo Pix, avise no WhatsApp e seu plano é ativado.", C_MUTED, 255, 0, 1);
    return;
  }

  int cw = 1100, ch = 850, cx = (W - cw) / 2, cy = 115;
  gfx_rrect(s, cx, cy, cw, ch, 28, C_PANEL, 250, 0);
  gfx_rrect(s, cx, cy, cw, ch, 28, C_WHITE, 26, 2);
  auth_money(sel->price, money, sizeof(money));
  if (ui->shop == 2) {
    text_draw_in(s, ui->fb, 44, cx, cw, cy + 76, "Pague com Pix", C_ACCENT, 255, 0, 1);
    snprintf(t, sizeof(t), "Plano %s  ·  %s  ·  %d dias", sel->name, money, sel->days);
    text_draw_in(s, ui->fb, 30, cx, cw, cy + 124, t, C_TEXT, 255, 0, 1);
    draw_qr_text(ui, sel->pix, 1, W / 2, cy + 150, 500);
    text_draw_in(s, ui->f, 25, cx, cw, cy + 700, "Abra o app do seu banco  \xE2\x80\xBA  Pix  \xE2\x80\xBA  Ler QR Code", C_MUTED, 255, 0, 1);
    shop_button(ui, cx + (cw - 440) / 2, cy + 735, 440, 72, "JÁ PAGUEI");
  } else {
    char user[80], msgtxt[200], enc[400], url[520];
    snprintf(user, sizeof(user), "%s", ui->username);
    char lower[64];
    snprintf(lower, sizeof(lower), "%s", sel->name);
    for (char *c = lower; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    snprintf(msgtxt, sizeof(msgtxt), "Olá meu usuário é %s e eu assinei o plano %s", user, lower);
    auth_urlencode(msgtxt, enc, sizeof(enc));
    snprintf(url, sizeof(url), "https://wa.me/%s?text=%s", pl->wa[0] ? pl->wa : "5511984970539", enc);
    text_draw_in(s, ui->fb, 44, cx, cw, cy + 76, "Avise que você pagou", C_ACCENT, 255, 0, 1);
    text_draw_in(s, ui->f, 28, cx, cw, cy + 122, "Escaneie com o celular e envie a mensagem pronta no WhatsApp.", C_TEXT, 255, 0, 1);
    draw_qr_text(ui, url, 1, W / 2, cy + 150, 480);
    text_fit(ui->f, 24, msgtxt, cw - 100, t, sizeof(t));
    text_draw_in(s, ui->f, 24, cx, cw, cy + 680, t, C_MUTED, 255, 0, 1);
    text_draw_in(s, ui->f, 24, cx, cw, cy + 715, "Seu plano é ativado em alguns minutos. Depois, toque no botão abaixo.", C_MUTED, 255, 0, 1);
    shop_button(ui, cx + (cw - 560) / 2, cy + 745, 560, 72, "JÁ ENVIEI · ATUALIZAR PLANO");
  }
}

static void draw_upd_help(UI *ui) {
  Image *s = ui->s;
  gfx_fill(s, 0, 0, W, H, C_BLACK, 200);
  int cw = 1280, ch = 700, cx = (W - cw) / 2, cy = 150;
  gfx_rrect(s, cx, cy, cw, ch, 28, C_PANEL, 250, 0);
  gfx_rrect(s, cx, cy, cw, ch, 28, C_WHITE, 26, 2);
  char t[120];
  snprintf(t, sizeof(t), "Nova versão do app: JK Patchs %s", ui->cat ? ui->cat->app.version : "");
  text_draw_in(s, ui->fb, 44, cx, cw, cy + 84, t, C_ACCENT, 255, 0, 1);
  char l[5][200];
  snprintf(l[0], sizeof(l[0]), "1.  Feche este app (botão PS  \xE2\x80\xBA  Fechar).");
  snprintf(l[1], sizeof(l[1]), "2.  Na tela inicial, selecione JK Patchs, aperte OPTIONS e escolha Apagar.");
  snprintf(l[2], sizeof(l[2]), "3.  Abra Configurações  \xE2\x80\xBA  Debug Settings  \xE2\x80\xBA  Game  \xE2\x80\xBA  Package Installer.");
  snprintf(l[3], sizeof(l[3]), "4.  Escolha %s (já está baixado no console) e instale.", ui->upd_file[0] ? ui->upd_file : "o arquivo do app");
  snprintf(l[4], sizeof(l[4]), "5.  Abra o JK Patchs de novo. Seu login continua salvo.");
  for (int i = 0; i < 5; i++) text_draw_in(s, ui->f, 30, cx + 70, cw - 140, cy + 190 + i * 74, l[i], C_TEXT, 255, 0, 0);
  text_draw_in(s, ui->f, 24, cx, cw, cy + ch - 40, "Aperte  O  para fechar. O aviso volta enquanto você não instalar.", C_MUTED, 255, 0, 1);
}

static void draw_guide(UI *ui) {
  Image *s = ui->s;
  gfx_fill(s, 0, 0, W, H, C_BG, 255);
  gfx_fill(s, 0, 0, W, 96, C_PANEL, 255);
  gfx_fill(s, 0, 96, W, 1, C_WHITE, 26);
  char t[120];
  text_fit(ui->fb, 34, ui->gtitle, 1100, t, sizeof(t));
  text_draw(s, ui->fb, 34, MARGIN, 62, t, C_TEXT, 255, 0);
  if (ui->gmode == 1) {
    int w = 760, h = 260, x = (W - w) / 2, y = 400;
    gfx_rrect(s, x, y, w, h, 28, C_PANEL, 245, 0);
    gfx_rrect(s, x, y, w, h, 28, C_WHITE, 26, 2);
    draw_spinner(ui, W / 2, y + 86);
    text_draw_in(s, ui->fb, 32, x, w, y + 178, "Carregando guia...", C_TEXT, 255, 0, 1);
    text_draw_in(s, ui->f, 24, W - MARGIN - 700, 700, 60, "O  Cancelar", C_MUTED, 255, 0, 2);
    return;
  }
  if (!ui->gimg) return;
  int vh = G_VIEW_H;
  int scrolls = ui->gimg->h > vh;
  text_draw_in(s, ui->f, 24, W - MARGIN - 760, 760, 60,
               scrolls ? "\xE2\x96\xB2 \xE2\x96\xBC  Rolar      L1  R1  P\xC3\xA1gina      O  Voltar" : "O  Voltar",
               C_MUTED, 255, 0, 2);
  int maxs = scrolls ? ui->gimg->h - vh : 0;
  if (ui->gscroll > maxs) ui->gscroll = maxs;
  Image v;                                                         /* janela vertical: as linhas sao contiguas */
  v.w = ui->gimg->w;
  v.px = ui->gimg->px + (size_t)ui->gscroll * (size_t)ui->gimg->w;
  v.h = ui->gimg->h - ui->gscroll;
  if (v.h > vh) v.h = vh;
  int x = (W - v.w) / 2;
  int y = G_TOP + (scrolls ? 0 : (vh - v.h) / 2);
  gfx_blit(s, &v, x, y, 0, 255);
  if (scrolls) {
    int tx = W - 36;
    gfx_rrect(s, tx, G_TOP, 10, vh, 5, C_PANEL2, 255, 0);
    int bh = vh * vh / ui->gimg->h;
    if (bh < 60) bh = 60;
    int by = G_TOP + (vh - bh) * ui->gscroll / maxs;
    gfx_rrect(s, tx, by, 10, bh, 5, C_ACCENT, 255, 0);
  }
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
  if (ui->gmode && ui->mode == MODE_CATALOG) { draw_guide(ui); return; }      /* guia em tela cheia */
  Image *back = (ui->mode == MODE_CATALOG && ui->fbg) ? ui->fbg : ui->bg;
  if (back) gfx_blit_surface(ui->s, back);
  else gfx_fill(ui->s, 0, 0, W, H, C_BG, 255);
  draw_topbar(ui);
  if (ui->mode == MODE_CATALOG) {
    if (ui->detail) draw_detail(ui); else draw_vitrine(ui);
    draw_message_line(ui);
    if (ui->shop) draw_shop(ui);
    if (ui->upd_help) draw_upd_help(ui);
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
