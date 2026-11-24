/* v1.43: plugins opcionais (aba PLUGINS da tela JK-MOD'S). Ver jkplug.h. */
#include "jkplug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>

#define JKX_STATE "/data/jk/plugins.txt"
#define JKX_INI   "/data/GoldHEN/plugins.ini"
#define MAXIT     12          /* itens por linha JKX. Pequeno de proposito: Item vai na pilha (a thread de download tem 256 KB; v1.43 travava ao desinstalar com 48) */

typedef struct { char id[24], cusa[16], dest[128]; } JBoard;

static pthread_mutex_t jx_mu = PTHREAD_MUTEX_INITIALIZER;
static char jx_root[128] = "";
static CatJkx jx_def[CAT_MAX_JKX];
static int jx_n = 0;
static JBoard jx_b[CAT_MAX_PLB];
static int jx_nb = 0;

/* ------------------------------------------------------------------ arquivos */
static void P(const char *p, char *out, int cap) { snprintf(out, (size_t)cap, "%s%s", jx_root, p); }

static long long fsize(const char *p) {
  char q[400]; P(p, q, sizeof(q));
  int fd = open(q, O_RDONLY);
  if (fd < 0) return -1;
  long long n = (long long)lseek(fd, 0, SEEK_END);
  close(fd);
  return n;
}
static char *readf(const char *p, long long *len, long long max) {
  char q[400]; P(p, q, sizeof(q));
  int fd = open(q, O_RDONLY);
  if (fd < 0) return NULL;
  long long n = (long long)lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  if (n < 0 || n > max) { close(fd); return NULL; }
  char *b = (char *)malloc((size_t)n + 1);
  long long got = 0;
  while (b && got < n) { int r = (int)read(fd, b + got, (size_t)(n - got)); if (r <= 0) break; got += r; }
  close(fd);
  if (!b || got != n) { free(b); return NULL; }
  b[n] = 0; *len = n;
  return b;
}
static int writef(const char *p, const char *b, long long n) {
  char q[400], t[420]; P(p, q, sizeof(q));
  snprintf(t, sizeof(t), "%s.jktmp", q);
  int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) return -1;
  long long off = 0;
  while (off < n) { int w = (int)write(fd, b + off, (size_t)(n - off)); if (w <= 0) break; off += w; }
  close(fd);
  if (off != n) { unlink(t); return -1; }
  unlink(q);
  if (rename(t, q) == 0) return 0;
  /* sem rename: grava direto */
  fd = open(q, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) { unlink(t); return -1; }
  off = 0;
  while (off < n) { int w = (int)write(fd, b + off, (size_t)(n - off)); if (w <= 0) break; off += w; }
  close(fd);
  unlink(t);
  return off == n ? 0 : -1;
}
static int rm(const char *p) { char q[400]; P(p, q, sizeof(q)); return unlink(q); }
static void logf_(JkxLog log, const char *a, const char *b) {
  if (!log) return;
  char m[600]; snprintf(m, sizeof(m), "%s%s", a, b ? b : "");
  log(m);
}

/* ------------------------------------------------------------------ catalogo */
void jkx_migrate(void);
void jkx_set_root(const char *root) { snprintf(jx_root, sizeof(jx_root), "%s", root ? root : ""); }

void jkx_set_catalog(const Catalog *c) {
  pthread_mutex_lock(&jx_mu);
  jx_n = 0; jx_nb = 0;
  if (c) {
    for (int i = 0; i < c->njkx && i < CAT_MAX_JKX; i++) jx_def[jx_n++] = c->jkx[i];
    for (int i = 0; i < c->nplb && i < CAT_MAX_PLB; i++) {
      snprintf(jx_b[jx_nb].id, sizeof(jx_b[0].id), "%s", c->plb[i].id);
      snprintf(jx_b[jx_nb].cusa, sizeof(jx_b[0].cusa), "%s", c->plb[i].cusa);
      snprintf(jx_b[jx_nb].dest, sizeof(jx_b[0].dest), "%s", c->plb[i].dest);
      jx_nb++;
    }
  }
  pthread_mutex_unlock(&jx_mu);
  jkx_migrate();
}

static const JBoard *board_by_id(const char *id) {
  for (int i = 0; i < jx_nb; i++) if (!strcmp(jx_b[i].id, id)) return &jx_b[i];
  return NULL;
}

int jkx_for_board(const CatJkx *x, const char *board) {          /* "mega,brasfoot" contem board? */
  const char *s = x->boards;
  size_t L = strlen(board);
  while (*s) {
    while (*s == ',' || *s == ' ') s++;
    const char *e = s; while (*e && *e != ',') e++;
    if ((size_t)(e - s) == L && !strncmp(s, board, L)) return 1;
    s = e;
  }
  return 0;
}

/* ------------------------------------------------------------------ itens "pasta=url" */
typedef struct { char dir[128]; char url[1024]; char path[300]; long long want; int list, afr, nodp; } Item;   /* nodp: "@-" = pasta AFR mas fora do DpFileList (troca arquivo do jogo, ex.: dt12_g4.cpk) */

/* nome do arquivo do link: decodifica %XX e para no '?' (igual ao download do app) */
static void url_name(const char *u, char *out, int cap) {
  const char *b = strrchr(u, '/'); b = b ? b + 1 : u;
  int k = 0;
  while (*b && *b != '?' && k + 1 < cap) {
    unsigned char ch = (unsigned char)*b++;
    if (ch == '%' && b[0] && b[1]) {
      char hex[3] = { b[0], b[1], 0 }; char *end = NULL; long v = strtol(hex, &end, 16);
      if (end && *end == 0) { ch = (unsigned char)v; b += 2; }
    }
    int bad = (ch < 32 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|');
    out[k++] = bad ? '_' : (char)ch;
  }
  out[k] = 0;
  const char *qa = strstr(u, "?as=");                      /* v1.43: "?as=NOME" (mesmo nome que o download usa) */
  if (qa) {
    qa += 4; k = 0;
    while (*qa && *qa != '?' && k + 1 < cap) out[k++] = *qa++;
    out[k] = 0;
  }
}

static int items(const CatJkx *x, const char *dest, Item *it, int max) {
  char buf[sizeof(x->files)];
  snprintf(buf, sizeof(buf), "%s", x->files);
  int n = 0;
  for (char *s = buf; *s && n < max;) {
    char *bar = strchr(s, '|'); if (bar) *bar = 0;
    char *eq = strstr(s, "=https://");
    if (eq) {
      Item *t = &it[n];
      memset(t, 0, sizeof(*t));
      *eq = 0;
      if (!strcmp(s, "@") || !strcmp(s, "@-")) { snprintf(t->dir, sizeof(t->dir), "%s", dest); t->afr = 1; t->nodp = s[1] == '-'; }
      else snprintf(t->dir, sizeof(t->dir), "%s", s);
      snprintf(t->url, sizeof(t->url), "%s", eq + 1);
      t->list = strstr(t->url, "?list") != NULL;
      const char *q = strstr(t->url, "?once=");
      if (q) for (const char *d = q + 6; *d >= '0' && *d <= '9'; d++) t->want = t->want * 10 + (*d - '0');
      char nm[180]; url_name(t->url, nm, sizeof(nm));
      snprintf(t->path, sizeof(t->path), "%s/%s", t->dir, nm);
      if (!strncmp(t->dir, "/data/", 6) && !strstr(t->dir, "..")) n++;
    }
    if (!bar) break;
    s = bar + 1;
  }
  return n;
}

/* ------------------------------------------------------------------ estado */
static char st_buf[8192];
static int st_loaded = 0;

static void st_load(void) {
  if (st_loaded) return;
  long long n = 0;
  char *t = readf(JKX_STATE, &n, sizeof(st_buf) - 1);
  st_buf[0] = 0;
  if (t) { memcpy(st_buf, t, (size_t)n); st_buf[n] = 0; free(t); }
  st_loaded = 1;
}
static int st_get(const char *key, const char *cusa) {      /* sem trava (quem chama ja segura jx_mu) */
  st_load();
  char k[64]; snprintf(k, sizeof(k), "%s@%s=", key, cusa);
  int v = -1;
  for (char *l = st_buf; *l;) {
    if (!strncmp(l, k, strlen(k))) { char c = l[strlen(k)]; v = c == '1' ? 1 : c == '2' ? 2 : 0; }   /* v1.47: 2 = instalado mas DESATIVADO */
    char *e = strchr(l, '\n'); if (!e) break; l = e + 1;
  }
  return v;
}
int jkx_state(const char *key, const char *cusa) {
  pthread_mutex_lock(&jx_mu);
  int v = st_get(key, cusa);
  pthread_mutex_unlock(&jx_mu);
  return v;
}
static void st_set(const char *key, const char *cusa, int v);
static void st_set(const char *key, const char *cusa, int v) {
  st_load();
  char k[64]; snprintf(k, sizeof(k), "%s@%s=", key, cusa);
  char out[sizeof(st_buf)]; size_t o = 0;
  for (char *l = st_buf; *l;) {
    char *e = strchr(l, '\n'); size_t L = e ? (size_t)(e - l) + 1 : strlen(l);
    if (strncmp(l, k, strlen(k)) && L > 1 && o + L < sizeof(out) - 80) { memcpy(out + o, l, L); o += L; if (!e) out[o++] = '\n'; }
    if (!e) break;
    l = e + 1;
  }
  o += (size_t)snprintf(out + o, sizeof(out) - o, "%s%d\n", k, (v == 1 || v == 2) ? v : 0);
  memcpy(st_buf, out, o); st_buf[o] = 0;
  { char d[300]; P("/data/jk", d, sizeof(d)); mkdir(d, 0777); }
  writef(JKX_STATE, st_buf, (long long)o);
}
void jkx_state_set(const char *key, const char *cusa, int v) {
  pthread_mutex_lock(&jx_mu);
  st_set(key, cusa, v);
  pthread_mutex_unlock(&jx_mu);
}

/* ------------------------------------------------------------------ plugins.ini */
/* acha a linha do prx na secao. Devolve -1 sem secao, 0 sem linha, 1 com linha (ls/le = inicio/fim da linha) */
static int ini_find(char *t, long long n, const char *sec, const char *prx, char **ls, char **le, char **send) {
  char hdr[40]; snprintf(hdr, sizeof(hdr), "[%s]", sec);
  char *s = strstr(t, hdr);
  if (!s) return -1;
  char *end = strstr(s + 1, "\n[");
  if (!end) end = t + n; else end++;                        /* inicio da proxima secao */
  *send = end;
  size_t pl = strlen(prx);
  for (char *p = s; p < end; ) {
    char *q = p; while (*q == ';' || *q == ' ' || *q == '\t' || *q == '#') q++;
    char *e = p; while (e < end && *e != '\n') e++;
    if (!strncmp(q, prx, pl) && q[pl] == '=') { *ls = p; *le = e; return 1; }
    p = e < end ? e + 1 : end;
  }
  return 0;
}
static int ini_get(const char *sec, const char *prx) {
  long long n = 0; char *t = readf(JKX_INI, &n, 256 * 1024);
  if (!t) return -1;
  char *ls, *le, *se;
  int r = ini_find(t, n, sec, prx, &ls, &le, &se), v = -1;
  if (r == 1) {
    v = 0;
    if (*ls != ';' && *ls != '#') { char *eq = ls + strlen(prx); v = !strncmp(eq, "=true", 5) || !strncmp(eq, "=1", 2); }
  }
  free(t);
  return v;
}
static int ini_set(const char *sec, const char *prx, int v, JkxLog log) {
  long long n = 0; char *t = readf(JKX_INI, &n, 256 * 1024);
  if (!t) return 0;
  char *ls = NULL, *le = NULL, *se = NULL;
  int r = ini_find(t, n, sec, prx, &ls, &le, &se);
  if (r < 0 || (r == 0 && !v)) { free(t); return 0; }
  char line[200]; snprintf(line, sizeof(line), "%s=%s", prx, v ? "true" : "false");
  const char *nl = strstr(t, "\r\n") ? "\r\n" : "\n";
  char *o = (char *)malloc((size_t)n + 256);
  if (!o) { free(t); return 0; }
  size_t L = 0;
  if (r == 1) {
    char *lee = le; if (lee > ls && lee[-1] == '\r') lee--;
    if ((size_t)(lee - ls) == strlen(line) && !strncmp(ls, line, strlen(line))) { free(o); free(t); return 0; }
    memcpy(o, t, (size_t)(ls - t)); L = (size_t)(ls - t);
    memcpy(o + L, line, strlen(line)); L += strlen(line);
    memcpy(o + L, lee, (size_t)(t + n - lee)); L += (size_t)(t + n - lee);
  } else {                                                  /* poe depois da ultima linha com texto da secao */
    char hdr[40]; snprintf(hdr, sizeof(hdr), "[%s]", sec);
    char *h = strstr(t, hdr); char *he = h; while (*he && *he != '\n') he++;
    char *at = se;
    for (;;) {
      while (at > he && (at[-1] == '\n' || at[-1] == '\r' || at[-1] == ' ' || at[-1] == '\t')) at--;
      char *l0 = at; while (l0 > he && l0[-1] != '\n') l0--;
      if (l0 > he && (*l0 == ';' || *l0 == '#')) { at = l0; continue; }   /* comentario do fim (ex.: "; Demisan" da proxima) */
      break;
    }
    memcpy(o, t, (size_t)(at - t)); L = (size_t)(at - t);
    memcpy(o + L, nl, strlen(nl)); L += strlen(nl);
    memcpy(o + L, line, strlen(line)); L += strlen(line);
    memcpy(o + L, at, (size_t)(t + n - at)); L += (size_t)(t + n - at);
  }
  int ok = writef(JKX_INI, o, (long long)L) == 0;
  if (ok) { char m[300]; snprintf(m, sizeof(m), "Plugins: [%s] %s", sec, line); if (log) log(m); }
  free(o); free(t);
  return ok;
}

/* ------------------------------------------------------------------ DpFileList */
static int ends_cpk(const char *s) {
  size_t L = strlen(s);
  return L > 4 && (s[L-4] == '.') && (s[L-3] == 'c' || s[L-3] == 'C') && (s[L-2] == 'p' || s[L-2] == 'P') && (s[L-1] == 'k' || s[L-1] == 'K');
}
static int dp_should(const char *dir, const char *cusa, const char *name, int *managed) {
  /* 1 = listar, 0 = tirar; *managed = 0 se ninguem cuida desse nome */
  char p[400]; snprintf(p, sizeof(p), "%s/%s", dir, name);
  long long sz = fsize(p);
  *managed = 0;
  int want = 1, alguem = 0;
  for (int i = 0; i < jx_n; i++) {
    const CatJkx *x = &jx_def[i];
    Item it[MAXIT]; int n = items(x, dir, it, MAXIT);
    for (int k = 0; k < n; k++) {
      if (!it[k].afr || it[k].nodp || strcmp(it[k].path, p)) continue;
      *managed = 1;
      { int st = st_get(x->key, cusa); if ((jkx_fixed(x) && st != 2) || st == 1) alguem = 1; }   /* v1.47: DESATIVADO (2) sai do DpFileList, o arquivo fica */   /* v1.45: o CPK fica se QUALQUER plugin que usa ele estiver instalado (ex.: placares da Sula) */
    }
  }
  if (*managed && !alguem) want = 0;
  if (!strcmp(name, "VTA.cpk") || !strcmp(name, "EstadioVarzea.cpk")) *managed = 1;   /* v1.38 */
  /* v1.43c: CPKs antigos do JK-MOD que o DpFileList dos botoes cita: sem o arquivo, sai da lista (nao precisa mais do "fake" de 8 KB) */
  static const char *OLD[] = { "JKPlacar.cpk", "JKPlacar2.cpk", "CazeTV.cpk", "JKNarrador.cpk", "JKML.cpk", "CPK CLIENTES.cpk", "CPK ML.cpk", NULL };
  for (int i = 0; OLD[i]; i++) if (!strcmp(name, OLD[i])) *managed = 1;
  if (*managed && sz <= 0) want = 0;                        /* nunca citar CPK que nao existe (o jogo trava) */
  return want;
}

void jkx_dp_fix(const char *dir, const char *cusa, JkxLog log) {
  static const char *PL[5] = { "JK_PLACAR_MUN.cpk", "JK_PLACAR_LIB.cpk", "JK_PLACAR_CDB.cpk", "JK_PLACAR_B.cpk", "JK_PLACAR_A.cpk" };
  char p[400];
  snprintf(p, sizeof(p), "%s/DpFileList.bin", dir);
  long long n = 0;
  unsigned char *d = (unsigned char *)readf(p, &n, 64 * 1024);
  if (!d) return;
  int cnt = (int)(d[4] | (d[5] << 8) | (d[6] << 16) | ((unsigned)d[7] << 24));
  if (cnt <= 0 || cnt > 200 || n < 16 + 48LL * cnt) { free(d); return; }
  static char old[240][32], nw[240][32];
  int no = 0, m = 0;
  for (int k = 0; k < cnt; k++) { memcpy(old[no], d + 16 + 48 * k, 32); old[no][31] = 0; no++; }
  free(d);
  pthread_mutex_lock(&jx_mu);
  for (int k = 0; k < no; k++) {                            /* 1) o que ja estava: tira JK_PLACAR e o que nao deve estar */
    int jk = 0, mg = 0;
    for (int j = 0; j < 5; j++) if (!strcmp(old[k], PL[j])) jk = 1;
    if (jk) continue;
    if (!dp_should(dir, cusa, old[k], &mg) && mg) continue;
    if (m < 230) memcpy(nw[m++], old[k], 32);
  }
  /* 2) CPKs dos plugins (e VTA/Varzea) que existem e faltam na lista */
  static const char *LEG[2] = { "VTA.cpk", "EstadioVarzea.cpk" };
  char cand[64][32]; int nc = 0;
  for (int e = 0; e < 2; e++) snprintf(cand[nc++], 32, "%s", LEG[e]);
  for (int i = 0; i < jx_n && nc < 64; i++) {
    Item it[MAXIT]; int ni = items(&jx_def[i], dir, it, MAXIT);
    for (int k = 0; k < ni && nc < 64; k++) {
      if (!it[k].afr || it[k].nodp) continue;
      const char *nm = strrchr(it[k].path, '/') + 1;
      if (!ends_cpk(nm) || strlen(nm) > 31) continue;
      int dup = 0; for (int c = 0; c < nc; c++) if (!strcmp(cand[c], nm)) dup = 1;
      if (!dup) snprintf(cand[nc++], 32, "%s", nm);
    }
  }
  for (int c = 0; c < nc && m < 230; c++) {
    int tem = 0, mg = 0;
    for (int k = 0; k < m; k++) if (!strcmp(nw[k], cand[c])) tem = 1;
    if (!tem && dp_should(dir, cusa, cand[c], &mg)) { memset(nw[m], 0, 32); snprintf(nw[m], 32, "%s", cand[c]); m++; }
  }
  pthread_mutex_unlock(&jx_mu);
  /* 3) placares JK no fim (o ultimo manda): MUN (se existir) e os 4 so se os 4 existirem */
  int ok4 = 1;
  for (int j = 1; j < 5; j++) { snprintf(p, sizeof(p), "%s/%s", dir, PL[j]); if (fsize(p) <= 0) ok4 = 0; }
  snprintf(p, sizeof(p), "%s/%s", dir, PL[0]);
  if (fsize(p) > 0) { memset(nw[m], 0, 32); snprintf(nw[m], 32, "%s", PL[0]); m++; }
  if (ok4) for (int j = 1; j < 5; j++) { memset(nw[m], 0, 32); snprintf(nw[m], 32, "%s", PL[j]); m++; }
  int dif = m != no;
  for (int k = 0; k < m && !dif; k++) if (strcmp(nw[k], old[k])) dif = 1;
  if (!dif) return;
  long long on = 48LL * m + 16;
  unsigned char *o = (unsigned char *)calloc(1, (size_t)on);
  if (!o) return;
  for (int k = 0; k < m; k++) {                             /* {u32 0, u32 N-k, u64 0, nome[32]} x N + 16 zeros */
    unsigned v = (unsigned)(m - k);
    unsigned char *e = o + 48 * k;
    e[4] = (unsigned char)v; e[5] = (unsigned char)(v >> 8);
    memcpy(e + 16, nw[k], 32);
  }
  snprintf(p, sizeof(p), "%s/DpFileList.bin", dir);
  if (writef(p, (const char *)o, on) == 0) {
    char msg[200]; snprintf(msg, sizeof(msg), "Plugins: DpFileList de %s com %d CPKs", dir, m);
    if (log) log(msg);
  }
  free(o);
}

/* ------------------------------------------------------------------ instalado / botoes */
static int files_ok(const CatJkx *x, const char *cusa, const char *dest) {
  Item it[MAXIT]; int n = items(x, dest, it, MAXIT);
  if (n <= 0) return 0;
  for (int k = 0; k < n; k++) {
    if (it[k].list) continue;
    long long sz = fsize(it[k].path);
    if (sz <= 0 || (it[k].want > 0 && sz != it[k].want)) return 0;
  }
  if (x->prx[0] && ini_get(cusa, x->prx) != 1) return 0;
  return 1;
}
static int installed_nolock(const CatJkx *x, const char *cusa, const char *dest) {
  int st = st_get(x->key, cusa);
  if (st != 1 && st != 2) return 0;                         /* v1.43: cada jogo tem o seu (Mega/Brasfoot x Demisan). v1.47: 2 = desativado */
  Item it[MAXIT]; int n = items(x, dest, it, MAXIT);
  if (n <= 0) return 0;
  for (int k = 0; k < n; k++) {
    if (it[k].list) continue;
    long long sz = fsize(it[k].path);
    if (sz <= 0 || (it[k].want > 0 && sz != it[k].want)) return 0;
  }
  if (st == 1 && x->prx[0] && ini_get(cusa, x->prx) != 1) return 0;
  return 1;
}
int jkx_installed(const CatJkx *x, const CatPlBoard *b) {
  if (!x || !b) return 0;
  pthread_mutex_lock(&jx_mu);
  int r = installed_nolock(x, b->cusa, b->dest);
  pthread_mutex_unlock(&jx_mu);
  return r;
}

int jkx_button(const CatJkx *x, const CatPlBoard *b, CatButton *out) {
  memset(out, 0, sizeof(*out));
  Item it[MAXIT]; int n = items(x, b->dest, it, MAXIT);
  if (n <= 0) return -1;
  snprintf(out->label, sizeof(out->label), "%s", x->name);
  snprintf(out->name, sizeof(out->name), "%s@%s", x->key, b->id);
  snprintf(out->content_id, sizeof(out->content_id), "JKX");
  snprintf(out->action, sizeof(out->action), "download");
  snprintf(out->url, sizeof(out->url), "%s", it[0].url);
  snprintf(out->dest, sizeof(out->dest), "%s", it[0].dir);
  size_t L = 0;
  for (int k = 1; k < n; k++) {
    int w = snprintf(out->extra + L, sizeof(out->extra) - L, "%s%s=%s", L ? "|" : "", it[k].dir, it[k].url);
    if (w < 0 || L + (size_t)w >= sizeof(out->extra)) return -1;
    L += (size_t)w;
  }
  return 0;
}
void jkx_button_off(const CatJkx *x, const CatPlBoard *b, CatButton *out) {
  memset(out, 0, sizeof(*out));
  snprintf(out->label, sizeof(out->label), "%s", x->name);
  snprintf(out->name, sizeof(out->name), "%s@%s", x->key, b->id);
  snprintf(out->content_id, sizeof(out->content_id), "JKX");
  snprintf(out->action, sizeof(out->action), "jkxoff");
  snprintf(out->url, sizeof(out->url), "jkx");
}

/* "chave@quadro" -> chave, quadro */
static const JBoard *split_kb(const char *kb, char *key, int cap) {
  const char *at = strrchr(kb, '@');
  if (!at) return NULL;
  int L = (int)(at - kb); if (L >= cap) L = cap - 1;
  memcpy(key, kb, (size_t)L); key[L] = 0;
  return board_by_id(at + 1);
}
void jkx_mark_install(const char *kb) {
  char key[40];
  pthread_mutex_lock(&jx_mu);
  const JBoard *b = split_kb(kb, key, sizeof(key));
  if (b) st_set(key, b->cusa, 1);
  pthread_mutex_unlock(&jx_mu);
}

/* a secao/pasta de outro jogo ainda usa o plugin? (jogo instalado = tem DpFileList na pasta AFR) */
static int used_elsewhere(const char *key, const char *cusa) {
  for (int i = 0; i < jx_n; i++) {
    const CatJkx *x = &jx_def[i];
    if (strcmp(x->key, key)) continue;
    for (int j = 0; j < jx_nb; j++) {
      const JBoard *b = &jx_b[j];
      if (!strcmp(b->cusa, cusa) || !jkx_for_board(x, b->id)) continue;
      char p[200]; snprintf(p, sizeof(p), "%s/DpFileList.bin", b->dest);
      if (fsize(p) <= 0) continue;
      { int st = st_get(key, b->cusa); if (st == 1 || st == 2) return 1; }
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ bloqueio no download
 * Os botoes do JK-MOD (placar, narracao, ML) ainda trazem os plugins opcionais no catalogo. O app 1.43 so baixa
 * esses arquivos se o plugin estiver INSTALADO pela aba (estado 1) naquele jogo. Assim os botoes baixam so o obrigatorio. */
int jkx_blocked(const char *path) {
  int hit = 0, ok = 0;   /* ok fica 0: ver abaixo */
  { const char *nm = strrchr(path, '/'); nm = nm ? nm + 1 : path;
    if (!strncmp(nm, "JK_PLACAR_", 10)) return 0; }        /* v1.46: placar do Monte o seu (ex.: JK_PLACAR_SULA) sempre baixa, mesmo sendo o padrao de um plugin */
  pthread_mutex_lock(&jx_mu);
  for (int i = 0; i < jx_n && !ok; i++) {
    const CatJkx *x = &jx_def[i];
    if (jkx_fixed(x)) continue;                             /* v1.43e: FIXO vem com os botoes */
    for (int j = 0; j < jx_nb && !ok; j++) {
      const JBoard *b = &jx_b[j];
      if (!jkx_for_board(x, b->id)) continue;
      Item it[MAXIT]; int n = items(x, b->dest, it, MAXIT);
      for (int k = 0; k < n; k++) {
        size_t dl = strlen(it[k].dir);
        int m = !strcmp(it[k].path, path) || (it[k].list && !strncmp(path, it[k].dir, dl) && path[dl] == '/');
        if (!m) continue;
        hit = 1;                                            /* v1.43b: botao normal nunca baixa plugin (so a aba Plugins) */
      }
    }
  }
  pthread_mutex_unlock(&jx_mu);
  return hit && !ok;
}

/* ------------------------------------------------------------------ v1.43d: cards FIXOS */
/* FIXO: vem junto com o placar (os botoes baixam), a aba so mostra. "arquivos" = FIXO (so o prx) ou "apagar" = FIXO (com os arquivos) */
int jkx_fixed(const CatJkx *x) { return x && (!strcmp(x->files, "FIXO") || !strncmp(x->del, "FIXO", 4)); }   /* "FIXO:texto" = aviso fixo no card (ex.: desativado) */
const char *jkx_aviso(const CatJkx *x) { return (x && !strncmp(x->del, "AVISO=https://", 14)) ? x->del + 6 : NULL; }   /* v1.44 */
const char *jkx_fixed_note(const CatJkx *x) { return (x && !strncmp(x->del, "FIXO:", 5) && x->del[5]) ? x->del + 5 : NULL; }

/* k-esima alternativa do prx ("a|b|c"). 0 = acabou */
static int prx_alt(const char *prx, int k, char *out, int cap) {
  const char *s = prx;
  for (int i = 0; i < k && s; i++) { s = strchr(s, '|'); if (s) s++; }
  if (!s || !*s) return 0;
  int n = 0;
  while (s[n] && s[n] != '|' && n + 1 < cap) { out[n] = s[n]; n++; }
  out[n] = 0;
  return n > 0;
}
int jkx_fixed_on(const CatJkx *x, const CatPlBoard *b) {
  if (!jkx_fixed(x) || !b || jkx_fixed_note(x)) return 0;
  int on = 1, alts = 0, any = 0; char a[128];
  pthread_mutex_lock(&jx_mu);
  for (int k = 0; prx_alt(x->prx, k, a, sizeof(a)); k++) { alts++; if (fsize(a) > 0 && ini_get(b->cusa, a) == 1) any = 1; }
  if (alts && !any) on = 0;
  Item it[MAXIT]; int n = items(x, b->dest, it, MAXIT);
  for (int k = 0; on && k < n; k++) {
    if (it[k].list) continue;
    long long sz = fsize(it[k].path);
    if (sz <= 0 || (it[k].want > 0 && sz != it[k].want)) on = 0;
  }
  if (!alts && n <= 0) on = 0;
  pthread_mutex_unlock(&jx_mu);
  return on;
}

static char jx_got[8][128];
static int jx_ngot = 0;
void jkx_note_download(const char *path) {
  if (!path) return;
  pthread_mutex_lock(&jx_mu);
  for (int i = 0; i < jx_n && jx_ngot < 8; i++) {
    if (strcmp(jx_def[i].files, "FIXO")) continue;          /* v1.43e: so Financas Vivas/Gol Fora; o resto respeita o plugins.ini do R2 */
    char a[128]; int hit = 0;                               /* so o 1o prx liga sozinho (os outros sao versoes antigas: gol fora) */
    if (prx_alt(jx_def[i].prx, 0, a, sizeof(a)) && !strcmp(a, path)) hit = 1;
    if (hit) { snprintf(jx_got[jx_ngot++], sizeof(jx_got[0]), "%s", path); break; }
  }
  pthread_mutex_unlock(&jx_mu);
}
static int has_alt(const CatJkx *x, const char *p) {
  char a[128];
  for (int k = 0; prx_alt(x->prx, k, a, sizeof(a)); k++) if (!strcmp(a, p)) return 1;
  return 0;
}
/* sem trava: quem chama segura jx_mu */
static void fixed_apply(JkxLog log) {
  for (int g = 0; g < jx_ngot; g++) {
    const char *p = jx_got[g];
    if (fsize(p) <= 0) continue;
    for (int i = 0; i < jx_n; i++) {
      const CatJkx *x = &jx_def[i];
      if (strcmp(x->files, "FIXO") || !has_alt(x, p)) continue;
      for (int j = 0; j < jx_nb; j++) {
        if (!jkx_for_board(x, jx_b[j].id)) continue;
        const char *cusa = jx_b[j].cusa;
        if (ini_get(cusa, p) != 1) ini_set(cusa, p, 1, log);
        for (int o = 0; o < jx_n; o++) {                   /* mesma chave, outro patch do mesmo jogo: desliga o prx dele */
          const CatJkx *y = &jx_def[o];
          if (o == i || strcmp(y->files, "FIXO") || strcmp(y->key, x->key)) continue;
          int same = 0;
          for (int t = 0; t < jx_nb; t++) if (!strcmp(jx_b[t].cusa, cusa) && jkx_for_board(y, jx_b[t].id)) same = 1;
          if (!same) continue;
          char a[128];
          for (int k = 0; prx_alt(y->prx, k, a, sizeof(a)); k++)
            if (strcmp(a, p) && !has_alt(x, a) && ini_get(cusa, a) == 1) ini_set(cusa, a, 0, log);
        }
      }
    }
  }
  jx_ngot = 0;
}

/* ------------------------------------------------------------------ depois de todo download */
/* v1.45: outro plugin instalado usa o mesmo arquivo da pasta AFR? (sem trava: quem chama segura jx_mu) */
static int afr_em_uso(const char *path, const char *dest, const char *cusa) {
  for (int i = 0; i < jx_n; i++) {
    const CatJkx *x = &jx_def[i];
    { int st = st_get(x->key, cusa); if (!jkx_fixed(x) && st != 1 && st != 2) continue; }   /* v1.47: desativado ainda usa o arquivo */
    Item it[MAXIT]; int n = items(x, dest, it, MAXIT);
    for (int k = 0; k < n; k++) if (it[k].afr && !strcmp(it[k].path, path)) return 1;
  }
  return 0;
}

/* v1.47: linha sem FIXO de uma chave que tem card FIXO neste jogo (ex.: 2a linha do Orcamento = pes21_orcamento_tela) */
static int companion(const CatJkx *x, const char *dest) {
  if (jkx_fixed(x)) return 0;
  for (int i = 0; i < jx_n; i++) {
    const CatJkx *y = &jx_def[i];
    if (y == x || !jkx_fixed(y) || strcmp(y->key, x->key)) continue;
    for (int j = 0; j < jx_nb; j++) if (!strcmp(jx_b[j].dest, dest) && jkx_for_board(y, jx_b[j].id)) return 1;
  }
  return 0;
}

void jkx_apply(JkxLog log) {
  pthread_mutex_lock(&jx_mu);
  fixed_apply(log);
  pthread_mutex_unlock(&jx_mu);
  /* jogos: os quadros do catalogo + os 2 conhecidos (catalogo antigo sem PLB) */
  JBoard g[CAT_MAX_PLB + 2]; int ng = 0;
  pthread_mutex_lock(&jx_mu);
  for (int j = 0; j < jx_nb; j++) {
    int dup = 0; for (int k = 0; k < ng; k++) if (!strcmp(g[k].dest, jx_b[j].dest)) dup = 1;
    if (!dup) g[ng++] = jx_b[j];
  }
  pthread_mutex_unlock(&jx_mu);
  static const char *KD[2][2] = { { "/data/GoldHEN/AFR/CUSA22062", "CUSA22062" }, { "/data/GoldHEN/AFR/CUSA18676", "CUSA18676" } };
  for (int e = 0; e < 2; e++) {
    int dup = 0; for (int k = 0; k < ng; k++) if (!strcmp(g[k].dest, KD[e][0])) dup = 1;
    if (!dup) { memset(&g[ng], 0, sizeof(g[0])); snprintf(g[ng].dest, sizeof(g[0].dest), "%s", KD[e][0]); snprintf(g[ng].cusa, sizeof(g[0].cusa), "%s", KD[e][1]); ng++; }
  }
  for (int k = 0; k < ng; k++) {
    pthread_mutex_lock(&jx_mu);
    for (int i = 0; i < jx_n; i++) {
      const CatJkx *x = &jx_def[i];
      int aplica = 0;
      for (int j = 0; j < jx_nb; j++) if (!strcmp(jx_b[j].dest, g[k].dest) && jkx_for_board(x, jx_b[j].id)) aplica = 1;
      if (!aplica) continue;
      int st = st_get(x->key, g[k].cusa);
      if (jkx_fixed(x) || companion(x, g[k].dest)) {          /* v1.47: card FIXO (e a 2a linha dele): so o Ativado/Desativado da aba */
        if (x->prx[0] && !strchr(x->prx, '|') && !jkx_fixed_note(x)) {
          if (st == 2 && ini_get(g[k].cusa, x->prx) == 1) ini_set(g[k].cusa, x->prx, 0, log);
          if (st == 1 && !jkx_fixed(x) && fsize(x->prx) > 0 && ini_get(g[k].cusa, x->prx) != 1) ini_set(g[k].cusa, x->prx, 1, log);
        }
        continue;
      }
      if (x->prx[0]) {
        /* so carrega no jogo em que foi instalado pela aba (o plugins.ini dos botoes vem com tudo =true) */
        if ((st != 1 || fsize(x->prx) <= 0) && ini_get(g[k].cusa, x->prx) == 1) ini_set(g[k].cusa, x->prx, 0, log);
        if (st == 1 && fsize(x->prx) > 0 && ini_get(g[k].cusa, x->prx) != 1) ini_set(g[k].cusa, x->prx, 1, log);
      }
      if (st == 0) {                                        /* desinstalado: nada dele fica na pasta AFR */
        Item it[MAXIT]; int n = items(x, g[k].dest, it, MAXIT);
        for (int t = 0; t < n; t++) if (it[t].afr && fsize(it[t].path) >= 0 && !afr_em_uso(it[t].path, g[k].dest, g[k].cusa) && rm(it[t].path) == 0) logf_(log, "Plugins: apagado ", it[t].path);
      }
    }
    pthread_mutex_unlock(&jx_mu);
    jkx_dp_fix(g[k].dest, g[k].cusa, log);
  }
}

/* ------------------------------------------------------------------ desinstalar */
int jkx_uninstall(const char *kb, JkxListFn list, JkxLog log, char *msg, int cap) {
  char key[40];
  pthread_mutex_lock(&jx_mu);
  const JBoard *bp = split_kb(kb, key, sizeof(key));
  if (!bp) { pthread_mutex_unlock(&jx_mu); snprintf(msg, (size_t)cap, "Plugin desconhecido."); return -1; }
  JBoard b = *bp;
  st_set(key, b.cusa, 0);
  int other = used_elsewhere(key, b.cusa), apagados = 0, comum = 0;
  char name[64] = "";
  for (int i = 0; i < jx_n; i++) {
    const CatJkx *x = &jx_def[i];
    if (strcmp(x->key, key) || !jkx_for_board(x, b.id)) continue;
    snprintf(name, sizeof(name), "%s", x->name);
    if (x->prx[0]) ini_set(b.cusa, x->prx, 0, log);
    Item it[MAXIT]; int n = items(x, b.dest, it, MAXIT);
    for (int k = 0; k < n; k++) {
      if (!it[k].afr) comum = 1;
      if (!it[k].afr && other) continue;                   /* arquivo comum que o outro jogo ainda usa */
      if (it[k].list) {
        if (!list) continue;
        static char lp[96][200];
        pthread_mutex_unlock(&jx_mu);
        int nl = list(it[k].url, it[k].dir, lp, 96);
        pthread_mutex_lock(&jx_mu);
        for (int t = 0; t < nl; t++) if (rm(lp[t]) == 0) apagados++;
        continue;
      }
      if (rm(it[k].path) == 0) { apagados++; logf_(log, "Plugins: apagado ", it[k].path); }
    }
    if (x->del[0]) {                                        /* "apagar": arquivos a mais ("@/x" = pasta AFR) */
      char buf[sizeof(x->del)]; snprintf(buf, sizeof(buf), "%s", x->del);
      for (char *s = buf; *s;) {
        char *bar = strchr(s, '|'); if (bar) *bar = 0;
        char p[300];
        if (s[0] == '@') snprintf(p, sizeof(p), "%s%s", b.dest, s + 1); else snprintf(p, sizeof(p), "%s", s);
        if (!strncmp(p, "/data/", 6) && !strstr(p, "..") && (s[0] == '@' || !other) && rm(p) == 0) { apagados++; logf_(log, "Plugins: apagado ", p); }
        if (!bar) break;
        s = bar + 1;
      }
    }
  }
  pthread_mutex_unlock(&jx_mu);
  jkx_apply(log);
  snprintf(msg, (size_t)cap, "%s desinstalado (%d arquivos apagados)%s", name[0] ? name : key, apagados, (other && comum) ? ". O outro jogo continua com ele." : "");
  return 0;
}

int jkx_paths(const CatJkx *x, const CatPlBoard *b, char (*out)[200], int max) {
  Item it[MAXIT]; int n = items(x, b->dest, it, MAXIT), m = 0;
  for (int k = 0; k < n && m < max; k++) {
    if (it[k].list) snprintf(out[m++], 200, "%s/x", it[k].dir);
    else snprintf(out[m++], 200, "%s", it[k].path);
  }
  return m;
}

/* ------------------------------------------------------------------ migracao (1a vez no 1.43)
 * Quem ja tinha os plugins pelos botoes antigos: o que esta ligado no plugins.ini e com os arquivos certos
 * vira "instalado" (estado 1) naquele jogo. Roda uma vez so (marca "#migrado" no plugins.txt). */
void jkx_migrate(void) {
  pthread_mutex_lock(&jx_mu);
  st_load();
  if (jx_n <= 0 || jx_nb <= 0 || strstr(st_buf, "#migrado")) { pthread_mutex_unlock(&jx_mu); return; }
  for (int j = 0; j < jx_nb; j++) {
    const JBoard *b = &jx_b[j];
    char p[200]; snprintf(p, sizeof(p), "%s/DpFileList.bin", b->dest);
    if (fsize(p) <= 0) continue;                              /* jogo sem patch instalado */
    for (int i = 0; i < jx_n; i++) {
      const CatJkx *x = &jx_def[i];
      if (!jkx_for_board(x, b->id) || st_get(x->key, b->cusa) != -1) continue;
      if (files_ok(x, b->cusa, b->dest)) st_set(x->key, b->cusa, 1);
    }
  }
  size_t L = strlen(st_buf);
  if (L + 12 < sizeof(st_buf)) {
    snprintf(st_buf + L, sizeof(st_buf) - L, "#migrado\n");
    { char d[300]; P("/data/jk", d, sizeof(d)); mkdir(d, 0777); }
    writef(JKX_STATE, st_buf, (long long)strlen(st_buf));
  }
  pthread_mutex_unlock(&jx_mu);
}

/* ------------------------------------------------------------------ v1.47: ATIVADO / DESATIVADO */
int jkx_toggleable(const CatJkx *x) {
  if (!x || jkx_fixed_note(x)) return 0;
  if (x->prx[0] && !strchr(x->prx, '|')) return 1;          /* um prx so (Gol Fora tem 2 alternativas: fica de fora) */
  Item it[MAXIT]; int n = items(x, "/data/x", it, MAXIT);
  for (int k = 0; k < n; k++) if (it[k].afr && !it[k].nodp && ends_cpk(it[k].path)) return 1;   /* CPK do DpFileList */
  return 0;
}
int jkx_enabled(const CatJkx *x, const CatPlBoard *b) {
  if (!x || !b) return 0;
  pthread_mutex_lock(&jx_mu);
  int st = st_get(x->key, b->cusa);
  pthread_mutex_unlock(&jx_mu);
  return st != 2;
}
int jkx_fixed_present(const CatJkx *x, const CatPlBoard *b) {
  /* card FIXO "no console": o prx de cada linha da chave neste quadro existe (Gol Fora: qualquer alternativa).
   * Sem prx (Vestiario): os arquivos da linha. Nao olha o plugins.ini (desativado continua "no console"). */
  if (!x || !b) return 0;
  int ok = 1, nprx = 0; char a[128];
  pthread_mutex_lock(&jx_mu);
  for (int i = 0; i < jx_n; i++) {
    const CatJkx *y = &jx_def[i];
    if (strcmp(y->key, x->key) || !jkx_for_board(y, b->id) || !y->prx[0]) continue;
    int any = 0;
    for (int k = 0; prx_alt(y->prx, k, a, sizeof(a)); k++) if (fsize(a) > 0) any = 1;
    nprx++; if (!any) ok = 0;
  }
  if (!nprx) {
    Item it[MAXIT]; int n = items(x, b->dest, it, MAXIT);
    if (n <= 0) ok = 0;
    for (int k = 0; ok && k < n; k++) if (!it[k].list && fsize(it[k].path) <= 0) ok = 0;
  }
  pthread_mutex_unlock(&jx_mu);
  return ok;
}
int jkx_fixed_active(const CatJkx *x, const CatPlBoard *b) {   /* v1.47: o prx do card esta ligado (sem prx: nao foi desativado) */
  if (!x || !b) return 0;
  char a[128]; int alts = 0, on = 0;
  pthread_mutex_lock(&jx_mu);
  for (int k = 0; prx_alt(x->prx, k, a, sizeof(a)); k++) { alts++; if (fsize(a) > 0 && ini_get(b->cusa, a) == 1) on = 1; }
  if (!alts) on = st_get(x->key, b->cusa) != 2;
  pthread_mutex_unlock(&jx_mu);
  return on;
}
int jkx_fixed_files(const CatJkx *x, const CatPlBoard *b) {
  if (!x || !b) return 0;
  Item it[MAXIT]; return items(x, b->dest, it, MAXIT) > 0;
}
int jkx_toggle(const CatJkx *x, const CatPlBoard *b, int on, JkxLog log) {
  if (!x || !b) return -1;
  pthread_mutex_lock(&jx_mu);
  int nv = on ? 1 : 2;
  st_set(x->key, b->cusa, nv);
  if (nv == 1) {                                             /* liga o prx das linhas DESTE quadro (Financas Vivas: mega x brasfoot) */
    for (int i = 0; i < jx_n; i++) {
      const CatJkx *y = &jx_def[i];
      if (strcmp(y->key, x->key) || !jkx_for_board(y, b->id) || !y->prx[0] || strchr(y->prx, '|')) continue;
      if (fsize(y->prx) > 0 && ini_get(b->cusa, y->prx) != 1) ini_set(b->cusa, y->prx, 1, log);
    }
  }
  pthread_mutex_unlock(&jx_mu);
  jkx_apply(log);
  return nv;
}
