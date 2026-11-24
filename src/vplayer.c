/* Player de video .jkv (veja vplayer.h). Duas threads: uma le o arquivo e decodifica (JPEG + ADPCM) para uma fila;
 * a outra consome a fila, entrega o quadro para a UI e toca o audio (o audio bloqueando e quem dita o ritmo). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "vplayer.h"

#define RING 6
#define GRAIN 256
#define MAX_SPF48 8192

typedef struct {
  Image *img;                       /* NULL = repete o quadro anterior */
  int16_t pcm[MAX_SPF48];
  int n48;
  int eof;
} Item;

static VpPlatform P;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static volatile int cancel_flag, paused, pct_done, muted;   /* v1.47: muted = toca sem som (video do card) */
static int state = VP_IDLE, alive = 0;
static char err_msg[200];
static int vw, vh, fps = 15, nframes, pos_frames;
static Image *latest;
static char cached_url[700];
static int cached_ok;
static char cached_path[300];          /* v1.47b: arquivo do video em cache_url */
static volatile int keep_next;          /* v1.47b: o proximo vp_load guarda o video */

/* v1.47b: caminho do video guardado: cache_dir/<hash do link>.jkv */
static void keep_path(const char *url, char *out, int cap) {
  unsigned long long h = 1469598103934665603ULL;
  for (const char *p = url; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
  snprintf(out, (size_t)cap, "%s/%016llx.jkv", P.cache_dir, h);
}
static int jkv_ok(const char *path) {      /* arquivo completo (o download grava .part e renomeia no fim) */
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  unsigned char h[4]; int ok = fread(h, 1, 4, f) == 4 && !memcmp(h, "JKV1", 4);
  fclose(f);
  return ok;
}

static Item *ring[RING];
static int r_head, r_count;          /* protegidos por mu */
static volatile int prod_done;

static const int STEP[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
  80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
  876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
  5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
  32767};
static const int IDX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static void logm(const char *m) { if (P.log) P.log(m); }
static void slp(unsigned us) { if (P.sleep_us) P.sleep_us(us); }
static void set_state(int s) { pthread_mutex_lock(&mu); state = s; pthread_mutex_unlock(&mu); }
static void fail(const char *msg) {
  pthread_mutex_lock(&mu);
  snprintf(err_msg, sizeof(err_msg), "%s", msg);
  state = VP_ERROR;
  pthread_mutex_unlock(&mu);
  logm(msg);
}

/* Bloco ADPCM -> n amostras de 24 kHz. */
static void adpcm_decode(const unsigned char *b, int blen, int n, int16_t *out) {
  if (blen < 4) { memset(out, 0, (size_t)n * 2); return; }
  int pred = (int16_t)(b[0] | (b[1] << 8));
  int idx = b[2] > 88 ? 88 : b[2];
  out[0] = (int16_t)pred;
  int pos = 4;
  for (int i = 1; i < n; i++) {
    int code;
    int k = i - 1;
    if (pos + (k >> 1) >= blen) { out[i] = (int16_t)pred; continue; }
    unsigned char by = b[pos + (k >> 1)];
    code = (k & 1) ? (by >> 4) : (by & 15);
    int step = STEP[idx];
    int d = step >> 3;
    if (code & 4) d += step;
    if (code & 2) d += step >> 1;
    if (code & 1) d += step >> 2;
    pred += (code & 8) ? -d : d;
    if (pred > 32767) pred = 32767;
    if (pred < -32768) pred = -32768;
    idx += IDX[code & 7];
    if (idx < 0) idx = 0;
    if (idx > 88) idx = 88;
    out[i] = (int16_t)pred;
  }
}

static unsigned rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

typedef struct { FILE *f; int spf; } Prod;

static void ring_free_all(void) {
  pthread_mutex_lock(&mu);
  while (r_count > 0) {
    Item *it = ring[r_head];
    r_head = (r_head + 1) % RING;
    r_count--;
    if (it) { if (it->img) img_free(it->img); free(it); }
  }
  pthread_mutex_unlock(&mu);
}

static void *producer(void *arg) {
  Prod *p = (Prod *)arg;
  unsigned char *buf = NULL;
  size_t cap = 0;
  int16_t s24[MAX_SPF48 / 2 + 2];
  for (;;) {
    if (cancel_flag) break;
    pthread_mutex_lock(&mu);
    int full = r_count >= RING;
    pthread_mutex_unlock(&mu);
    if (full) { slp(3000); continue; }

    unsigned char h[8];
    Item *it = (Item *)calloc(1, sizeof(Item));
    if (!it) break;
    if (fread(h, 1, 8, p->f) != 8) { it->eof = 1; goto push; }
    unsigned jl = rd32(h), al = rd32(h + 4);
    if (jl > (8u << 20) || al > 65536u) { it->eof = 1; goto push; }
    if (jl + al > cap) {
      unsigned char *nb = (unsigned char *)realloc(buf, jl + al);
      if (!nb) { it->eof = 1; goto push; }
      buf = nb; cap = jl + al;
    }
    if (fread(buf, 1, jl + al, p->f) != jl + al) { it->eof = 1; goto push; }
    it->img = img_decode(buf, (int)jl);
    adpcm_decode(buf + jl, (int)al, p->spf, s24);
    for (int i = 0; i < p->spf; i++) {                      /* 24 kHz -> 48 kHz (interpolacao linear) */
      int a = s24[i], b2 = (i + 1 < p->spf) ? s24[i + 1] : a;
      it->pcm[2 * i] = (int16_t)a;
      it->pcm[2 * i + 1] = (int16_t)((a + b2) / 2);
    }
    it->n48 = p->spf * 2;
  push:
    pthread_mutex_lock(&mu);
    ring[(r_head + r_count) % RING] = it;
    r_count++;
    pthread_mutex_unlock(&mu);
    if (it->eof) break;
  }
  free(buf);
  prod_done = 1;
  return NULL;
}

static Item *pop(void) {
  Item *it = NULL;
  pthread_mutex_lock(&mu);
  if (r_count > 0) {
    it = ring[r_head];
    r_head = (r_head + 1) % RING;
    r_count--;
  }
  pthread_mutex_unlock(&mu);
  return it;
}

static void *player_thread(void *arg) {
  char url[700];
  snprintf(url, sizeof(url), "%s", (const char *)arg + 1);
  int keep = ((const char *)arg)[0] == 'K' && P.cache_dir && P.cache_dir[0];
  free(arg);
  char path[300];
  if (keep) keep_path(url, path, sizeof(path)); else snprintf(path, sizeof(path), "%s", P.tmp_path);
  FILE *f = NULL;
  int audio = 0;
  pthread_t pt;
  int have_pt = 0;
  Prod prod;
  char e[200] = "";

  if (keep && jkv_ok(path)) {                                 /* v1.47b: ja guardado no console */
    snprintf(cached_url, sizeof(cached_url), "%s", url); snprintf(cached_path, sizeof(cached_path), "%s", path); cached_ok = 1;
  }
  if (!(cached_ok && !strcmp(cached_url, url) && !strcmp(cached_path, path))) {
    cached_ok = 0;
    set_state(VP_DOWNLOADING);
    pct_done = 0;
    char part[310]; snprintf(part, sizeof(part), "%s%s", path, keep ? ".part" : "");
    if (!P.download || P.download(url, part, &pct_done, &cancel_flag, e, sizeof(e)) != 0) {
      if (keep) remove(part);
      if (cancel_flag) goto done;
      fail(e[0] ? e : "Não foi possível baixar o vídeo.");
      goto done;
    }
    if (keep) {
      remove(path);
      if (rename(part, path) != 0) { remove(part); fail("Não foi possível guardar o vídeo."); goto done; }
      char m[400]; snprintf(m, sizeof(m), "Video: guardado em %s", path); logm(m);
    }
    snprintf(cached_url, sizeof(cached_url), "%s", url);
    snprintf(cached_path, sizeof(cached_path), "%s", path);
    cached_ok = 1;
  }
  if (cancel_flag) goto done;

  f = fopen(path, "rb");
  unsigned char hd[24];
  if (!f || fread(hd, 1, 20, f) != 20 || memcmp(hd, "JKV1", 4) != 0) {
    cached_ok = 0;
    if (f) { fclose(f); f = NULL; }
    if (keep) remove(path);                                   /* v1.47b: guardado estragado: baixa de novo na proxima */
    fail("Arquivo de vídeo inválido (precisa ser .jkv; veja tools/mp4_to_jkv.py).");
    goto done;
  }
  {
    int w = hd[4] | (hd[5] << 8), h = hd[6] | (hd[7] << 8);
    int fp = hd[8] | (hd[9] << 8), ar = hd[10] | (hd[11] << 8);
    int nf = (int)rd32(hd + 12), spf = (int)rd32(hd + 16);
    if (w < 16 || h < 16 || w > 1920 || h > 1080 || fp < 1 || fp > 60 || ar != 24000 || spf < 64 || spf * 2 > MAX_SPF48) {
      cached_ok = 0;
      if (keep) { fclose(f); f = NULL; remove(path); }
      fail("Cabeçalho do vídeo inválido.");
      goto done;
    }
    pthread_mutex_lock(&mu);
    vw = w; vh = h; fps = fp; nframes = nf; pos_frames = 0;
    pthread_mutex_unlock(&mu);
    prod.f = f; prod.spf = spf;
  }

  if (P.audio_open && P.audio_open() == 0) audio = 1;
  else logm("Video: sem audio (nao abriu a saida de audio).");
  prod_done = 0;
  ring_free_all();
  {
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1024 * 1024);
    if (pthread_create(&pt, &at, producer, &prod) != 0) {
      pthread_attr_destroy(&at);
      fail("Não foi possível iniciar o vídeo.");
      goto done;
    }
    pthread_attr_destroy(&at);
    have_pt = 1;
  }
  set_state(VP_PLAYING);

  {
    static int16_t stage[MAX_SPF48 + GRAIN * 2];
    int sn = 0;
    unsigned frame_us = 1000000u / (unsigned)fps;
    for (;;) {
      if (cancel_flag) break;
      if (paused) { slp(20000); continue; }
      Item *it = pop();
      if (!it) {
        if (prod_done) {
          pthread_mutex_lock(&mu);
          int empty = r_count == 0;
          pthread_mutex_unlock(&mu);
          if (empty) break;
        }
        slp(2000);
        continue;
      }
      if (it->eof) { free(it); break; }
      if (it->img) {
        pthread_mutex_lock(&mu);
        if (latest) img_free(latest);
        latest = it->img;
        it->img = NULL;
        pos_frames++;
        pthread_mutex_unlock(&mu);
      } else {
        pthread_mutex_lock(&mu); pos_frames++; pthread_mutex_unlock(&mu);
      }
      if (audio) {
        if (muted) memset(stage + sn, 0, (size_t)it->n48 * 2); else memcpy(stage + sn, it->pcm, (size_t)it->n48 * 2);
        sn += it->n48;
        int off = 0;
        while (sn - off >= GRAIN && !cancel_flag && !paused) { P.audio_write(stage + off, GRAIN); off += GRAIN; }
        if (off < sn) memmove(stage, stage + off, (size_t)(sn - off) * 2);
        sn -= off;
      } else {
        slp(frame_us);
      }
      free(it);
    }
    if (audio && sn > 0 && !cancel_flag) {
      memset(stage + sn, 0, (size_t)(GRAIN - sn % GRAIN) * 2);
      P.audio_write(stage, ((sn + GRAIN - 1) / GRAIN) * GRAIN);
    }
  }
  if (!cancel_flag) set_state(VP_ENDED);

done:
  cancel_flag = 1;                          /* garante que o produtor pare */
  if (have_pt) pthread_join(pt, NULL);
  if (audio && P.audio_close) P.audio_close();
  if (f) fclose(f);
  ring_free_all();
  pthread_mutex_lock(&mu);
  int was_cancel_only = (state == VP_DOWNLOADING || state == VP_PLAYING);
  if (was_cancel_only) state = VP_IDLE;
  alive = 0;
  pthread_mutex_unlock(&mu);
  return NULL;
}

void vp_init(const VpPlatform *p) { P = *p; }

int vp_load(const char *url) {
  pthread_mutex_lock(&mu);
  if (alive) { pthread_mutex_unlock(&mu); return -1; }
  alive = 1;
  state = VP_DOWNLOADING;
  err_msg[0] = 0;
  vw = vh = 0;
  if (latest) { img_free(latest); latest = NULL; }
  pthread_mutex_unlock(&mu);
  cancel_flag = 0;
  paused = 0;
  pct_done = 0;
  size_t ul = strlen(url);
  char *u = (char *)malloc(ul + 2);
  if (u) { u[0] = keep_next ? 'K' : '-'; memcpy(u + 1, url, ul + 1); }
  keep_next = 0;
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int r = u ? pthread_create(&t, &at, player_thread, u) : -1;
  pthread_attr_destroy(&at);
  if (r != 0) {
    free(u);
    pthread_mutex_lock(&mu); alive = 0; state = VP_IDLE; pthread_mutex_unlock(&mu);
    return -1;
  }
  return 0;
}

int vp_load_keep(const char *url) {
  keep_next = 1;
  int r = vp_load(url);
  keep_next = 0;
  return r;
}

void vp_stop(void) {
  cancel_flag = 1;
  pthread_mutex_lock(&mu);
  if (!alive) state = VP_IDLE;
  pthread_mutex_unlock(&mu);
}
void vp_set_paused(int p) { paused = p ? 1 : 0; }
void vp_set_mute(int m) { muted = m ? 1 : 0; }
int vp_paused(void) { return paused; }
int vp_state(void) { pthread_mutex_lock(&mu); int s = state; pthread_mutex_unlock(&mu); return s; }
int vp_busy(void) { pthread_mutex_lock(&mu); int a = alive; pthread_mutex_unlock(&mu); return a; }
int vp_progress(void) { return pct_done; }
void vp_error(char *out, int cap) { pthread_mutex_lock(&mu); snprintf(out, (size_t)cap, "%s", err_msg); pthread_mutex_unlock(&mu); }
int vp_aspect(int *w, int *h) {
  pthread_mutex_lock(&mu);
  int ok = vw > 0;
  if (ok) { *w = vw; *h = vh; }
  pthread_mutex_unlock(&mu);
  return ok;
}
Image *vp_take_frame(void) {
  pthread_mutex_lock(&mu);
  Image *im = latest;
  latest = NULL;
  pthread_mutex_unlock(&mu);
  return im;
}
double vp_position(void) { pthread_mutex_lock(&mu); double p = (double)pos_frames / fps; pthread_mutex_unlock(&mu); return p; }
double vp_duration(void) { pthread_mutex_lock(&mu); double d = nframes > 0 ? (double)nframes / fps : 0; pthread_mutex_unlock(&mu); return d; }
