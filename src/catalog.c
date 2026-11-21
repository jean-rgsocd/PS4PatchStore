#include "catalog.h"

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static void copy(char *dst, int cap, const char *src) {
  if (!src) src = "";
  strncpy(dst, src, (size_t)cap - 1);
  dst[cap - 1] = 0;
}

/* Divide a linha em campos separados por TAB (altera a linha). Retorna o numero de campos. */
static int split(char *line, char **f, int max) {
  int n = 0;
  f[n++] = line;
  for (char *p = line; *p && n < max; p++) {
    if (*p == '\t') { *p = 0; f[n++] = p + 1; }
  }
  return n;
}

int catalog_version_num(const char *v) {
  int major = 0, minor = 0, i = 0;
  if (!v || !v[0]) return 0;
  while (v[i] >= '0' && v[i] <= '9') major = major * 10 + (v[i++] - '0');
  if (v[i] == '.') {
    i++;
    int digits = 0;
    while (v[i] >= '0' && v[i] <= '9' && digits < 2) { minor = minor * 10 + (v[i++] - '0'); digits++; }
    if (digits == 1) minor *= 10;                       /* "1.5" = 1.50 */
  }
  return major * 100 + minor;
}

int catalog_pl_comp(const char *s) {
  if (!s) return -1;
  if (!strcmp(s, "A")) return 0;
  if (!strcmp(s, "B")) return 1;
  if (!strcmp(s, "CDB")) return 2;
  if (!strcmp(s, "LIB")) return 3;
  return -1;
}

static CatPlBoard *pl_board(Catalog *c, const char *id) {
  for (int i = 0; i < c->nplb; i++) if (!strcmp(c->plb[i].id, id)) return &c->plb[i];
  return NULL;
}
static int pl_opt(const CatPlBoard *b, const char *id) {
  for (int i = 0; i < b->nopt; i++) if (!strcmp(b->opt[i].id, id)) return i;
  return -1;
}

static int is_extra_list(const char *s) {
  return !strncmp(s, "https://", 8) || !strncmp(s, "/data/", 6) || !strncmp(s, "/user/appmeta/", 14);
}

int catalog_parse(Catalog *c, char *text) {
  memset(c, 0, sizeof(*c));
  copy(c->cat[0].id, sizeof(c->cat[0].id), "all");
  copy(c->cat[0].label, sizeof(c->cat[0].label), "Todas");
  c->ncat = 1;

  CatGame *g = NULL;
  char *save = text;
  while (*save) {
    char *line = save;
    char *nl = line;
    while (*nl && *nl != '\n') nl++;
    save = *nl ? nl + 1 : nl;
    *nl = 0;
    if (nl > line && nl[-1] == '\r') nl[-1] = 0;
    if (!line[0] || line[0] == '#') continue;

    char *f[12];
    int n = split(line, f, 12);

    if (!strcmp(f[0], "LOCK")) {
      c->locked = 1;
    } else if (!strcmp(f[0], "APP") && n >= 6) {
      copy(c->app.version, sizeof(c->app.version), f[1]);
      copy(c->app.url, sizeof(c->app.url), f[2]);
      copy(c->app.content_id, sizeof(c->app.content_id), f[3]);
      c->app.size = strtoull(f[4], NULL, 10);
      copy(c->app.type, sizeof(c->app.type), f[5]);
    } else if (!strcmp(f[0], "MOD") && n >= 4) {
      copy(c->mod.version, sizeof(c->mod.version), f[1]);
      copy(c->mod.url, sizeof(c->mod.url), f[2]);
      c->mod.size = strtoull(f[3], NULL, 10);
    } else if (!strcmp(f[0], "CAT") && n >= 3) {
      if (!strcmp(f[1], "all")) {                       /* so renomeia o "todas" */
        copy(c->cat[0].label, sizeof(c->cat[0].label), f[2]);
      } else if (c->ncat < CAT_MAX_CATS) {
        copy(c->cat[c->ncat].id, sizeof(c->cat[0].id), f[1]);
        copy(c->cat[c->ncat].label, sizeof(c->cat[0].label), f[2]);
        c->ncat++;
      }
    } else if (!strcmp(f[0], "GAME") && n >= 7) {
      if (c->ngame >= CAT_MAX_GAMES) { g = NULL; continue; }
      g = &c->game[c->ngame++];
      copy(g->category, sizeof(g->category), f[1]);
      copy(g->cover, sizeof(g->cover), f[2]);
      copy(g->tag, sizeof(g->tag), f[3]);
      copy(g->kicker, sizeof(g->kicker), f[4]);
      copy(g->title, sizeof(g->title), f[5]);
      copy(g->desc, sizeof(g->desc), f[6]);
      if (n >= 8 && !strncmp(f[7], "https://", 8)) copy(g->video, sizeof(g->video), f[7]);
    } else if (!strcmp(f[0], "BTN") && n >= 8 && g && g->nbtn < CAT_MAX_BTNS) {
      CatButton *b = &g->btn[g->nbtn++];
      copy(b->label, sizeof(b->label), f[1]);
      copy(b->name, sizeof(b->name), f[2]);
      copy(b->url, sizeof(b->url), f[3]);
      copy(b->content_id, sizeof(b->content_id), f[4]);
      b->size = strtoull(f[5], NULL, 10);
      copy(b->type, sizeof(b->type), f[6]);
      b->base = atoi(f[7]);
      copy(b->action, sizeof(b->action), (n >= 9 && f[8][0]) ? f[8] : "pkg");
      if (n >= 10) copy(b->dest, sizeof(b->dest), f[9]);
      if (n >= 11 && is_extra_list(f[10])) copy(b->extra, sizeof(b->extra), f[10]);
      if (n >= 12 && is_extra_list(f[11])) {                /* v1.37: extras novos (o app antigo nao le este campo) */
        size_t L = strlen(b->extra), L2 = strlen(f[11]);
        if (L + 1 + L2 < sizeof(b->extra)) {
          if (L) b->extra[L++] = '|';
          memcpy(b->extra + L, f[11], L2 + 1);
        }
      }
    } else if (!strcmp(f[0], "PLB") && n >= 7) {
      CatPlBoard *b = pl_board(c, f[1]);
      if (!b && c->nplb < CAT_MAX_PLB) b = &c->plb[c->nplb++];
      if (b) {
        copy(b->id, sizeof(b->id), f[1]);
        copy(b->title, sizeof(b->title), f[2]);
        copy(b->cusa, sizeof(b->cusa), f[3]);
        copy(b->dest, sizeof(b->dest), f[4]);
        copy(b->extra, sizeof(b->extra), is_extra_list(f[5]) ? f[5] : "");
        b->monte = atoi(f[6]);
      }
    } else if (!strcmp(f[0], "PLO") && n >= 9) {
      CatPlBoard *b = pl_board(c, f[1]);
      int comp = catalog_pl_comp(f[3]);
      if (b && comp >= 0 && b->nopt < CAT_MAX_PLO && pl_opt(b, f[2]) < 0) {
        CatPlOpt *o = &b->opt[b->nopt++];
        copy(o->id, sizeof(o->id), f[2]);
        o->comp = comp;
        copy(o->name, sizeof(o->name), f[4]);
        copy(o->url, sizeof(o->url), f[5]);
        o->size = strtoull(f[6], NULL, 10);
        copy(o->img, sizeof(o->img), f[7]);
        copy(o->logo, sizeof(o->logo), f[8]);
      }
    } else if (!strcmp(f[0], "PLP") && n >= 7) {
      CatPlBoard *b = pl_board(c, f[1]);
      if (b && b->npack < CAT_MAX_PLP) {
        CatPlPack p;
        int ok = 1;
        copy(p.name, sizeof(p.name), f[2]);
        for (int k = 0; k < CAT_PL_COMPS; k++) {
          int oi = pl_opt(b, f[3 + k]);
          if (oi < 0 || b->opt[oi].comp != k) ok = 0;   /* PLO precisa vir antes e ser da competicao certa */
          p.opt[k] = oi;
        }
        if (ok) b->pack[b->npack++] = p;
      }
    }
  }
  return c->ngame > 0 ? 0 : -2;
}

int catalog_load(Catalog *c, const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  off_t n = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  if (n <= 0 || n > 4 * 1024 * 1024) { close(fd); return -1; }
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) { close(fd); return -1; }
  off_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, buf + got, (size_t)(n - got));
    if (r <= 0) break;
    got += r;
  }
  close(fd);
  buf[got] = 0;
  int rc = catalog_parse(c, buf);
  free(buf);
  return rc;
}
