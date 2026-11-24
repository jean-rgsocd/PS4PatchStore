/* Player de video dentro do app: toca arquivos .jkv (quadros JPEG + audio IMA-ADPCM, veja tools/mp4_to_jkv.py).
 * Independente de plataforma: download, audio e espera vem de ganchos (VpPlatform) registrados pelo app. */
#ifndef VPLAYER_H
#define VPLAYER_H

#include <stdint.h>
#include "gfx.h"

typedef struct {
  /* Baixa url para dest. pct = 0..100 (atualizado durante o download); cancel != 0 interrompe.
   * Retorna 0 se ok; senao preenche err (texto para o usuario). */
  int  (*download)(const char *url, const char *dest, volatile int *pct, volatile int *cancel, char *err, int cap);
  int  (*audio_open)(void);                               /* 0 se ok (mono, 48 kHz, S16) */
  void (*audio_write)(const int16_t *pcm, int n);         /* n amostras mono 48 kHz; bloqueia ate tocar */
  void (*audio_close)(void);
  void (*sleep_us)(unsigned us);
  void (*log)(const char *msg);
  const char *tmp_path;                                   /* onde guardar o video baixado */
} VpPlatform;

enum { VP_IDLE = 0, VP_DOWNLOADING, VP_PLAYING, VP_ENDED, VP_ERROR };

void vp_init(const VpPlatform *p);
/* Comeca (download se preciso + reproducao). Falha (retorna -1) se ainda ha uma reproducao encerrando. */
int  vp_load(const char *url);
void vp_stop(void);                       /* cancela e para (volta a VP_IDLE quando as threads terminam) */
void vp_set_paused(int paused);
void vp_set_mute(int mute);             /* v1.47: 1 = sem som (o ritmo continua pelo audio) */
int  vp_paused(void);
int  vp_state(void);
int  vp_busy(void);                       /* 1 enquanto alguma thread do player ainda roda */
int  vp_progress(void);                   /* 0..100 durante o download */
void vp_error(char *out, int cap);        /* texto do erro (VP_ERROR) */
int  vp_aspect(int *w, int *h);           /* tamanho do video (apos abrir); 0 se ainda nao sabe */
/* Pega o quadro mais novo ainda nao entregue (o chamador passa a ser dono e libera com img_free). NULL se nao ha. */
Image *vp_take_frame(void);
double vp_position(void);                 /* segundos tocados */
double vp_duration(void);                 /* segundos totais (0 se desconhecido) */

#endif
