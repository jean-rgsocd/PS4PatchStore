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
  if (!strcmp(s, "MUN")) return 4;                          /* v1.43 */
  if (!strcmp(s, "SULA")) return 5;                         /* v1.45 */
  /* v1.43: "A+" "B+" "CDB+" "LIB+" = opcao nova so para o app novo (o 1.42 ignora): para testar antes de liberar */
  if (!strcmp(s, "A+")) return 0;
  if (!strcmp(s, "B+")) return 1;
  if (!strcmp(s, "CDB+")) return 2;
  if (!strcmp(s, "LIB+")) return 3;
  return -1;
}

void catalog_key(const char *in, char *out, int cap) {
  int n = 0;
  const unsigned char *p = (const unsigned char *)(in ? in : "");
  while (*p && n < cap - 1) {
    unsigned char c = *p++;
    if (c == 0xC3 && *p) {                                   /* Latin-1 em UTF-8: tira o acento */
      unsigned char d = *p++ | 0x20;                         /* maiuscula -> minuscula (0x80..0x9F -> 0xA0..0xBF) */
      char r = 0;
      if (d >= 0xA0 && d <= 0xA5) r = 'a'; else if (d == 0xA7) r = 'c'; else if (d >= 0xA8 && d <= 0xAB) r = 'e';
      else if (d >= 0xAC && d <= 0xAF) r = 'i'; else if (d == 0xB1) r = 'n'; else if (d >= 0xB2 && d <= 0xB6) r = 'o';
      else if (d >= 0xB9 && d <= 0xBC) r = 'u';
      if (r) out[n++] = r;
      continue;
    }
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[n++] = (char)c;
  }
  out[n] = 0;
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
    } else if ((!strcmp(f[0], "PLB") || !strcmp(f[0], "PLB2") || !strcmp(f[0], "PLB3")) && n >= 7) {   /* PLB2 (v1.38) / PLB3 (v1.39): quadros que as versoes antigas ignoram */
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
    } else if ((!strcmp(f[0], "JKN") || !strcmp(f[0], "JKC")) && n >= 3) {   /* v1.40 */
      if (c->nart < CAT_MAX_ART && !strncmp(f[2], "https://", 8)) {
        CatArt *a = &c->art[c->nart++];
        a->kind = f[0][2] == 'C';
        catalog_key(f[1], a->key, sizeof(a->key));
        copy(a->img, sizeof(a->img), f[2]);
      }
    } else if (!strcmp(f[0], "FAQ") && n >= 3) {                  /* v1.41 */
      if (c->nfaq < CAT_MAX_FAQ && !strncmp(f[2], "https://", 8)) {
        CatFaq *q = &c->faq[c->nfaq++];
        copy(q->title, sizeof(q->title), f[1]);
        copy(q->img, sizeof(q->img), f[2]);
        copy(q->video, sizeof(q->video), (n >= 4 && !strncmp(f[3], "https://", 8)) ? f[3] : "");
      }
    } else if (!strcmp(f[0], "JKX") && n >= 8) {                  /* v1.43 */
      if (c->njkx < CAT_MAX_JKX && f[1][0] && strlen(f[7]) < sizeof(c->jkx[0].files)) {
        CatJkx *x = &c->jkx[c->njkx++];
        copy(x->key, sizeof(x->key), f[1]);
        copy(x->name, sizeof(x->name), f[2]);
        copy(x->img, sizeof(x->img), !strncmp(f[3], "https://", 8) ? f[3] : "");
        copy(x->boards, sizeof(x->boards), f[4]);
        copy(x->prx, sizeof(x->prx), !strncmp(f[5], "/data/", 6) ? f[5] : "");
        copy(x->del, sizeof(x->del), strcmp(f[6], "-") ? f[6] : "");
        copy(x->files, sizeof(x->files), f[7]);
      }
    } else if (!strcmp(f[0], "JKG") && n >= 3) {
      if (c->njk < CAT_MAX_PLB) {
        copy(c->jk[c->njk].board, sizeof(c->jk[0].board), f[1]);
        copy(c->jk[c->njk].game, sizeof(c->jk[0].game), f[2]);
        c->njk++;
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
        for (int k = 0; k < 4; k++) {
          int oi = pl_opt(b, f[3 + k]);
          if (oi < 0 || b->opt[oi].comp != k) ok = 0;   /* PLO precisa vir antes e ser da competicao certa */
          p.opt[k] = oi;
        }
        p.opt[4] = -1; p.opt[5] = -1;                     /* v1.43: 5o placar (Mundial / Copas) e opcional; v1.45: 6o (Sula) so no Monte o seu */
        if (n >= 8 && f[7][0]) { int oi = pl_opt(b, f[7]); if (oi >= 0 && b->opt[oi].comp == 4) p.opt[4] = oi; }
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
