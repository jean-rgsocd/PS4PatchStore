#include "gfx.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>

/* stb_image: so PNG/JPEG, sem stdio, sem HDR (evita pow/ldexp), sem assert.h e sem TLS
 * (variaveis thread-local sao um risco num eboot homebrew). STATIC deixa o compilador
 * descartar o que nao e usado, entao nao dependemos de simbolos extras do libc. */
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)
#define STBI_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

/* stb_truetype: floor/ceil sem libm (so truncamento) e funcoes estaticas. */
static inline int gfx_ifloor(float x) { int i = (int)x; return (x < (float)i) ? i - 1 : i; }
static inline int gfx_iceil(float x)  { int i = (int)x; return (x > (float)i) ? i + 1 : i; }
#define STBTT_ifloor(x) gfx_ifloor((float)(x))
#define STBTT_iceil(x)  gfx_iceil((float)(x))
#define STBTT_assert(x) ((void)0)
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

/* ------------------------------------------------------------------ imagens */

Image *img_new(int w, int h) {
  Image *im = (Image *)malloc(sizeof(Image));
  if (!im) return NULL;
  im->w = w; im->h = h;
  im->px = (uint32_t *)calloc((size_t)w * h, 4);
  if (!im->px) { free(im); return NULL; }
  return im;
}

void img_free(Image *im) { if (im) { free(im->px); free(im); } }

static unsigned char *read_file(const char *path, int *len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  off_t n = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  if (n <= 0) { close(fd); return NULL; }
  unsigned char *b = (unsigned char *)malloc((size_t)n);
  if (!b) { close(fd); return NULL; }
  off_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, b + got, (size_t)(n - got));
    if (r <= 0) break;
    got += r;
  }
  close(fd);
  if (got != n) { free(b); return NULL; }
  *len = (int)n;
  return b;
}

Image *img_load(const char *path) {
  int len = 0, w, h, comp;
  unsigned char *file = read_file(path, &len);
  if (!file) return NULL;
  unsigned char *rgba = stbi_load_from_memory(file, len, &w, &h, &comp, 4);
  free(file);
  if (!rgba) return NULL;
  Image *im = img_new(w, h);
  if (im) {
    for (int i = 0; i < w * h; i++) {
      const unsigned char *p = rgba + 4 * i;
      im->px[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
  }
  stbi_image_free(rgba);
  return im;
}

static uint32_t sample_bilinear(const Image *s, float fx, float fy) {
  if (fx < 0) fx = 0;
  if (fy < 0) fy = 0;
  int x0 = (int)fx, y0 = (int)fy;
  int x1 = x0 + 1 < s->w ? x0 + 1 : s->w - 1;
  int y1 = y0 + 1 < s->h ? y0 + 1 : s->h - 1;
  if (x0 >= s->w) x0 = s->w - 1;
  if (y0 >= s->h) y0 = s->h - 1;
  int ax = (int)((fx - x0) * 256), ay = (int)((fy - y0) * 256);
  uint32_t c00 = s->px[y0 * s->w + x0], c10 = s->px[y0 * s->w + x1];
  uint32_t c01 = s->px[y1 * s->w + x0], c11 = s->px[y1 * s->w + x1];
  uint32_t out = 0;
  for (int sh = 0; sh < 32; sh += 8) {
    int a = (c00 >> sh) & 255, b = (c10 >> sh) & 255, c = (c01 >> sh) & 255, d = (c11 >> sh) & 255;
    int top = a * (256 - ax) + b * ax, bot = c * (256 - ax) + d * ax;
    int v = (top * (256 - ay) + bot * ay) >> 16;
    out |= (uint32_t)v << sh;
  }
  return out;
}

/* Reducao com media por caixa (area) seguida de bilinear: evita serrilhado em capas grandes. */
Image *img_scale(const Image *src, int w, int h) {
  Image *dst = img_new(w, h);
  if (!dst) return NULL;
  float sx = (float)src->w / w, sy = (float)src->h / h;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      if (sx <= 1.0f && sy <= 1.0f) {
        dst->px[y * w + x] = sample_bilinear(src, (x + 0.5f) * sx - 0.5f, (y + 0.5f) * sy - 0.5f);
      } else {
        int x0 = (int)(x * sx), x1 = (int)((x + 1) * sx), y0 = (int)(y * sy), y1 = (int)((y + 1) * sy);
        if (x1 <= x0) x1 = x0 + 1;
        if (y1 <= y0) y1 = y0 + 1;
        if (x1 > src->w) x1 = src->w;
        if (y1 > src->h) y1 = src->h;
        unsigned long sum[4] = {0, 0, 0, 0}, n = 0;
        for (int yy = y0; yy < y1; yy++)
          for (int xx = x0; xx < x1; xx++) {
            uint32_t c = src->px[yy * src->w + xx];
            sum[0] += c & 255; sum[1] += (c >> 8) & 255; sum[2] += (c >> 16) & 255; sum[3] += c >> 24;
            n++;
          }
        if (!n) n = 1;
        dst->px[y * w + x] = (uint32_t)(sum[0] / n) | ((uint32_t)(sum[1] / n) << 8) |
                             ((uint32_t)(sum[2] / n) << 16) | ((uint32_t)(sum[3] / n) << 24);
      }
    }
  }
  return dst;
}

Image *img_scale_cover(const Image *src, int w, int h) {
  /* recorta o centro de src na proporcao w:h e escala */
  float ar = (float)w / h, sar = (float)src->w / src->h;
  int cw = src->w, ch = src->h, cx = 0, cy = 0;
  if (sar > ar) { cw = (int)(src->h * ar); cx = (src->w - cw) / 2; }
  else          { ch = (int)(src->w / ar); cy = (src->h - ch) / 2; }
  Image crop = { cw, ch, NULL };
  Image *tmp = img_new(cw, ch);
  if (!tmp) return NULL;
  for (int y = 0; y < ch; y++) memcpy(tmp->px + y * cw, src->px + (y + cy) * src->w + cx, (size_t)cw * 4);
  (void)crop;
  Image *out = img_scale(tmp, w, h);
  img_free(tmp);
  return out;
}

/* ------------------------------------------------------------------ primitivas */

static inline uint32_t blend(uint32_t dst, uint32_t rgb, int a) {   /* a: 0..255 */
  if (a >= 255) return GFX_A | (rgb & 0xFFFFFF);
  if (a <= 0) return dst;
  int dr = (dst >> 16) & 255, dg = (dst >> 8) & 255, db = dst & 255;
  int sr = (rgb >> 16) & 255, sg = (rgb >> 8) & 255, sb = rgb & 255;
  int r = dr + (((sr - dr) * a + 127) / 255);
  int g = dg + (((sg - dg) * a + 127) / 255);
  int b = db + (((sb - db) * a + 127) / 255);
  return GFX_A | (r << 16) | (g << 8) | b;
}

static inline void put(Image *s, int x, int y, uint32_t rgb, int a) {
  if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) return;
  uint32_t *p = &s->px[y * s->w + x];
  *p = blend(*p, rgb, a);
}

void gfx_fill(Image *s, int x, int y, int w, int h, uint32_t rgb, int alpha) {
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + w > s->w ? s->w : x + w, y1 = y + h > s->h ? s->h : y + h;
  if (x0 >= x1 || y0 >= y1) return;
  if (alpha >= 255) {
    uint32_t v = GFX_A | (rgb & 0xFFFFFF);
    for (int yy = y0; yy < y1; yy++) {
      uint32_t *p = s->px + yy * s->w + x0;
      for (int xx = x0; xx < x1; xx++) *p++ = v;
    }
  } else {
    for (int yy = y0; yy < y1; yy++) {
      uint32_t *p = s->px + yy * s->w + x0;
      for (int xx = x0; xx < x1; xx++, p++) *p = blend(*p, rgb, alpha);
    }
  }
}

/* Sobreposicao de cor solida cujo alpha varia linearmente de a_top a a_bottom. */
void gfx_vfade(Image *s, int x, int y, int w, int h, uint32_t rgb, int a_top, int a_bottom) {
  for (int j = 0; j < h; j++) {
    int a = h > 1 ? a_top + (a_bottom - a_top) * j / (h - 1) : a_top;
    gfx_fill(s, x, y + j, w, 1, rgb, a);
  }
}

void gfx_vgradient(Image *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
  for (int j = 0; j < h; j++) {
    int t = h > 1 ? j * 255 / (h - 1) : 0;
    int r = (int)((top >> 16) & 255) + ((int)((bottom >> 16) & 255) - (int)((top >> 16) & 255)) * t / 255;
    int g = (int)((top >> 8) & 255) + ((int)((bottom >> 8) & 255) - (int)((top >> 8) & 255)) * t / 255;
    int b = (int)(top & 255) + ((int)(bottom & 255) - (int)(top & 255)) * t / 255;
    gfx_fill(s, x, y + j, w, 1, RGB(r, g, b), 255);
  }
}

/* Cobertura (0..255) de um pixel (centro px+.5,py+.5) dentro de um retangulo arredondado
 * de largura w, altura h (origem 0,0) e raio r. */
static int rr_cov(int px, int py, int w, int h, int r) {
  float cx = px + 0.5f, cy = py + 0.5f;
  float hx = w * 0.5f, hy = h * 0.5f;
  float qx = fabsf(cx - hx) - (hx - r), qy = fabsf(cy - hy) - (hy - r);
  float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
  float inside = qx > qy ? qx : qy; if (inside > 0) inside = 0;
  float d = sqrtf(ox * ox + oy * oy) + inside - r;
  float c = 0.5f - d;
  if (c <= 0) return 0;
  if (c >= 1) return 255;
  return (int)(c * 255);
}

void gfx_rrect(Image *s, int x, int y, int w, int h, int r, uint32_t rgb, int alpha, int ring) {
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  if (r < 0) r = 0;
  int band = r + (ring > 0 ? ring : 0);           /* faixa onde precisa calcular por pixel */
  for (int j = 0; j < h; j++) {
    int yy = y + j;
    if ((unsigned)yy >= (unsigned)s->h) continue;
    int edge_row = (j < band || j >= h - band);
    if (ring == 0 && !edge_row) {                  /* miolo: linha reta */
      gfx_fill(s, x, yy, w, 1, rgb, alpha);
      continue;
    }
    if (ring > 0 && !edge_row) {                   /* so as duas laterais */
      gfx_fill(s, x, yy, ring, 1, rgb, alpha);
      gfx_fill(s, x + w - ring, yy, ring, 1, rgb, alpha);
      continue;
    }
    for (int i = 0; i < w; i++) {
      int xx = x + i;
      int c;
      if (ring == 0) {
        c = (i >= r && i < w - r && j >= r && j < h - r) ? 255 : rr_cov(i, j, w, h, r);
      } else {
        int co = rr_cov(i, j, w, h, r);
        if (!co) continue;
        int ci = (i >= ring && i < w - ring && j >= ring && j < h - ring)
                     ? rr_cov(i - ring, j - ring, w - 2 * ring, h - 2 * ring, r > ring ? r - ring : 0)
                     : 0;
        c = co * (255 - ci) / 255;
      }
      if (c) put(s, xx, yy, rgb, alpha * c / 255);
    }
  }
}

void gfx_circle(Image *s, int cx, int cy, int r, uint32_t rgb, int alpha) {
  gfx_rrect(s, cx - r, cy - r, r * 2, r * 2, r, rgb, alpha, 0);
}

/* Anel com um arco "aberto" (spinner): desenha pontos em volta do centro. */
void gfx_ring_arc(Image *s, int cx, int cy, int r, int thick, uint32_t rgb, int alpha) {
  (void)thick;
  gfx_circle(s, cx, cy, r, rgb, alpha);
}

void gfx_blit(Image *s, const Image *im, int x, int y, int r, int alpha) {
  if (r * 2 > im->w) r = im->w / 2;
  if (r * 2 > im->h) r = im->h / 2;
  for (int j = 0; j < im->h; j++) {
    int yy = y + j;
    if ((unsigned)yy >= (unsigned)s->h) continue;
    for (int i = 0; i < im->w; i++) {
      int xx = x + i;
      if ((unsigned)xx >= (unsigned)s->w) continue;
      uint32_t c = im->px[j * im->w + i];
      int a = (int)(c >> 24);
      if (r > 0 && (i < r || i >= im->w - r) && (j < r || j >= im->h - r))
        a = a * rr_cov(i, j, im->w, im->h, r) / 255;
      a = a * alpha / 255;
      if (a) { uint32_t *p = &s->px[yy * s->w + xx]; *p = blend(*p, c & 0xFFFFFF, a); }
    }
  }
}

void gfx_blit_surface(Image *dst, const Image *src) {
  if (dst->w == src->w && dst->h == src->h) memcpy(dst->px, src->px, (size_t)dst->w * dst->h * 4);
}

/* ------------------------------------------------------------------ texto */

struct Font {
  stbtt_fontinfo info;
  unsigned char *data;
  int id;
};

typedef struct {
  uint64_t key;          /* (font<<40)|(px<<32)|cp ; 0 = vazio */
  unsigned char *bmp;
  int w, h, xoff, yoff, adv;
} Glyph;

#define GCACHE 4096
static Glyph gcache[GCACHE];
static int g_next_font = 1;

Font *font_load(const char *path) {
  int len = 0;
  unsigned char *data = read_file(path, &len);
  if (!data) return NULL;
  Font *f = (Font *)calloc(1, sizeof(Font));
  if (!f) { free(data); return NULL; }
  f->data = data;
  f->id = g_next_font++;
  if (!stbtt_InitFont(&f->info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
    free(data); free(f);
    return NULL;
  }
  return f;
}

int font_has(Font *f, int cp) { return stbtt_FindGlyphIndex(&f->info, cp) != 0; }

static int utf8_next(const char **ps) {
  const unsigned char *s = (const unsigned char *)*ps;
  int c = *s;
  if (!c) return 0;
  if (c < 0x80) { *ps += 1; return c; }
  if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { *ps += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
  if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    *ps += 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
  }
  if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
    *ps += 4; return ((c & 7) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
  }
  *ps += 1;
  return '?';
}

static Glyph *glyph(Font *f, int px, int cp) {
  uint64_t key = ((uint64_t)f->id << 40) | ((uint64_t)px << 32) | (uint32_t)cp;
  unsigned idx = (unsigned)((key * 11400714819323198485ull) >> 52) & (GCACHE - 1);
  for (int probe = 0; probe < GCACHE; probe++, idx = (idx + 1) & (GCACHE - 1)) {
    Glyph *g = &gcache[idx];
    if (g->key == key) return g;
    if (g->key == 0) {
      float scale = stbtt_ScaleForMappingEmToPixels(&f->info, (float)px);   /* px = tamanho de fonte (como CSS) */
      int gi = stbtt_FindGlyphIndex(&f->info, cp);
      if (gi == 0 && cp != '?') { gi = stbtt_FindGlyphIndex(&f->info, ' '); }
      int adv, lsb;
      stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
      g->key = key;
      g->adv = (int)(adv * scale + 0.5f);
      g->bmp = stbtt_GetGlyphBitmap(&f->info, scale, scale, gi, &g->w, &g->h, &g->xoff, &g->yoff);
      return g;
    }
  }
  return NULL;
}

int text_width(Font *f, int px, const char *s, int spacing) {
  int w = 0;
  int cp;
  while ((cp = utf8_next(&s)) != 0) {
    Glyph *g = glyph(f, px, cp);
    if (g) w += g->adv + spacing;
  }
  return w > 0 ? w - spacing : 0;
}

int text_draw(Image *s, Font *f, int px, int x, int baseline, const char *utf8,
              uint32_t rgb, int alpha, int spacing) {
  int pen = x, cp;
  while ((cp = utf8_next(&utf8)) != 0) {
    Glyph *g = glyph(f, px, cp);
    if (!g) continue;
    if (g->bmp) {
      for (int j = 0; j < g->h; j++) {
        int yy = baseline + g->yoff + j;
        if ((unsigned)yy >= (unsigned)s->h) continue;
        for (int i = 0; i < g->w; i++) {
          int a = g->bmp[j * g->w + i];
          if (!a) continue;
          put(s, pen + g->xoff + i, yy, rgb, a * alpha / 255);
        }
      }
    }
    pen += g->adv + spacing;
  }
  return pen - x - (pen > x ? spacing : 0);
}

int text_draw_in(Image *s, Font *f, int px, int x, int w, int baseline, const char *utf8,
                 uint32_t rgb, int alpha, int spacing, int align) {
  int tw = text_width(f, px, utf8, spacing);
  int ox = x;
  if (align == 1) ox = x + (w - tw) / 2;
  else if (align == 2) ox = x + w - tw;
  return text_draw(s, f, px, ox, baseline, utf8, rgb, alpha, spacing);
}

void text_fit(Font *f, int px, const char *utf8, int maxw, char *out, int cap) {
  if (text_width(f, px, utf8, 0) <= maxw) { strncpy(out, utf8, (size_t)cap - 1); out[cap - 1] = 0; return; }
  int n = (int)strlen(utf8);
  if (n > cap - 4) n = cap - 4;
  while (n > 0) {
    while (n > 0 && ((unsigned char)utf8[n] & 0xC0) == 0x80) n--;   /* nao cortar no meio de um caractere */
    memcpy(out, utf8, (size_t)n);
    strcpy(out + n, "...");
    if (text_width(f, px, out, 0) <= maxw) return;
    n--;
  }
  strcpy(out, "...");
}

int text_wrap(Image *s, Font *f, int px, int x, int baseline, int maxw, int line_h,
              int max_lines, const char *utf8, uint32_t rgb, int alpha) {
  char buf[1024], line[1024];
  strncpy(buf, utf8, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  const char *p = buf;
  int lines = 0;
  while (*p && lines < max_lines) {
    while (*p == ' ') p++;
    if (!*p) break;
    /* maior prefixo de palavras inteiras que cabe em maxw */
    const char *best_end = NULL, *q = p;
    while (*q) {
      const char *w = q;
      while (*w && *w != ' ') w++;
      int n = (int)(w - p);
      if (n >= (int)sizeof(line)) break;
      memcpy(line, p, (size_t)n); line[n] = 0;
      if (text_width(f, px, line, 0) > maxw) break;
      best_end = w;
      q = w;
      while (*q == ' ') q++;
    }
    const char *rest = best_end ? best_end : p;
    while (*rest == ' ') rest++;
    int last_line = (lines == max_lines - 1);
    if (!best_end || (last_line && *rest)) {
      /* palavra maior que a linha, ou texto que nao cabe nas linhas permitidas: corta com ... */
      text_fit(f, px, p, maxw, line, sizeof(line));
      text_draw(s, f, px, x, baseline + lines * line_h, line, rgb, alpha, 0);
      return lines + 1;
    }
    int n = (int)(best_end - p);
    memcpy(line, p, (size_t)n); line[n] = 0;
    text_draw(s, f, px, x, baseline + lines * line_h, line, rgb, alpha, 0);
    lines++;
    p = rest;
  }
  return lines;
}
