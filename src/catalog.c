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

    if (!strcmp(f[0], "CAT") && n >= 3) {
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
    } else if (!strcmp(f[0], "BTN") && n >= 8 && g && g->nbtn < CAT_MAX_BTNS) {
      CatButton *b = &g->btn[g->nbtn++];
      copy(b->label, sizeof(b->label), f[1]);
      copy(b->name, sizeof(b->name), f[2]);
      copy(b->url, sizeof(b->url), f[3]);
      copy(b->content_id, sizeof(b->content_id), f[4]);
      b->size = strtoull(f[5], NULL, 10);
      copy(b->type, sizeof(b->type), f[6]);
      b->base = atoi(f[7]);
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
