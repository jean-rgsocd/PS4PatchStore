/* Camada grafica 2D por software (independente de plataforma).
 * Superficie ARGB8888 com alpha fixo 0x80 (formato esperado pelo VideoOut do PS4). */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

#define GFX_A 0x80000000u

typedef struct { int w, h; uint32_t *px; } Image;     /* px: 0xAARRGGBB (alpha reto) */

typedef struct Font Font;

Image *img_new(int w, int h);
void   img_free(Image *im);
Image *img_load(const char *path);                       /* PNG/JPG; NULL se falhar */
Image *img_scale(const Image *src, int w, int h);        /* bilinear/area */
Image *img_scale_cover(const Image *src, int w, int h);  /* preenche w x h cortando o excesso */

#define RGB(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))

void gfx_fill(Image *s, int x, int y, int w, int h, uint32_t rgb, int alpha);
void gfx_vfade(Image *s, int x, int y, int w, int h, uint32_t rgb, int a_top, int a_bottom);
void gfx_vgradient(Image *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
/* Retangulo arredondado. ring=0 preenche; ring>0 desenha so a borda de espessura ring. */
void gfx_rrect(Image *s, int x, int y, int w, int h, int r, uint32_t rgb, int alpha, int ring);
void gfx_circle(Image *s, int cx, int cy, int r, uint32_t rgb, int alpha);
void gfx_ring_arc(Image *s, int cx, int cy, int r, int thick, uint32_t rgb, int alpha);
/* Copia uma imagem (com alpha) com cantos arredondados de raio r. */
void gfx_blit(Image *s, const Image *im, int x, int y, int r, int alpha);
void gfx_blit_surface(Image *dst, const Image *src);     /* copia total (mesmo tamanho) */

Font *font_load(const char *path);
int  font_has(Font *f, int codepoint);                    /* 1 se a fonte tem o glifo */
/* Largura em pixels de um texto UTF-8 (spacing = espaco extra entre letras). */
int  text_width(Font *f, int px, const char *utf8, int spacing);
/* Desenha com a linha de base em y. Retorna a largura desenhada. */
int  text_draw(Image *s, Font *f, int px, int x, int baseline, const char *utf8,
               uint32_t rgb, int alpha, int spacing);
/* Centralizado/alinhado a direita dentro de [x, x+w]. align: 0 esq, 1 centro, 2 dir. */
int  text_draw_in(Image *s, Font *f, int px, int x, int w, int baseline, const char *utf8,
                  uint32_t rgb, int alpha, int spacing, int align);
/* Quebra de linha por palavras. Retorna numero de linhas desenhadas. */
int  text_wrap(Image *s, Font *f, int px, int x, int baseline, int maxw, int line_h,
               int max_lines, const char *utf8, uint32_t rgb, int alpha);
/* Copia utf8 em out cortando com "..." para caber em maxw. */
void text_fit(Font *f, int px, const char *utf8, int maxw, char *out, int cap);

#endif
