/* PS4 Patch Store - app NATIVO (OpenOrbis).
 * Desenha a interface direto na tela (1920x1080, por software), le o controle e instala os pacotes
 * pelo mesmo payload do DirectPackageInstaller usado pela versao navegador.
 *
 * Arquivos esperados em /app0: catalog.dat, directpayload.bin e as pastas assets/fonts e assets/images.
 * Log de diagnostico: /data/ps4store.log */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>

#include "gfx.h"
#include "ui.h"
#include "catalog.h"
#include "install.h"

/* Heap do libc: a UI usa ~60 MB (tela, fundo, capas, JPEG decodificado). */
uint64_t sceLibcHeapSize = 256ull * 1024 * 1024;

#define FW        1920
#define FH        1080
#define NBUF      2
#define ASSETS    "/app0/assets"
#define CATALOG   "/app0/catalog.dat"
#define TICK_US   16000

/* ------------------------------------------------------------------ video */
static int video = -1;
static void *fbuf[NBUF];
static OrbisKernelEqueue flipq;
static int cur_buf = 0;
static int64_t frame_id = 0;

static int video_init(void) {
  video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
  if (video < 0) { install_log("ERRO: sceVideoOutOpen falhou"); return -1; }

  if (sceKernelCreateEqueue(&flipq, "ps4store flip") < 0) { install_log("ERRO: equeue"); return -1; }
  sceVideoOutAddFlipEvent(flipq, video, 0);

  size_t align = 0x200000;
  size_t need = (size_t)NBUF * FW * FH * 4;
  size_t size = (need + align - 1) / align * align;

  off_t off = 0;
  if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, align, 3, &off) < 0) {
    install_log("ERRO: memoria de video (AllocateDirectMemory)");
    return -1;
  }
  void *mem = NULL;
  if (sceKernelMapDirectMemory(&mem, size, 0x33, 0, off, align) < 0 || !mem) {
    install_log("ERRO: memoria de video (MapDirectMemory)");
    return -1;
  }
  for (int i = 0; i < NBUF; i++) fbuf[i] = (char *)mem + (size_t)i * FW * FH * 4;

  OrbisVideoOutBufferAttribute attr;
  memset(&attr, 0, sizeof(attr));
  sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, FW, FH, FW);
  if (sceVideoOutRegisterBuffers(video, 0, fbuf, NBUF, &attr) != 0) {
    install_log("ERRO: sceVideoOutRegisterBuffers");
    return -1;
  }
  sceVideoOutSetFlipRate(video, 0);
  return 0;
}

/* Copia a superficie (memoria normal) para o buffer de video e troca. A memoria de video e "write-combined":
 * escrever em sequencia e rapido, ler e muito lento. Por isso a UI desenha na heap e so copiamos. */
static void present(const Image *s) {
  memcpy(fbuf[cur_buf], s->px, (size_t)FW * FH * 4);
  frame_id++;
  sceVideoOutSubmitFlip(video, cur_buf, ORBIS_VIDEO_OUT_FLIP_VSYNC, frame_id);

  /* espera o flip terminar (com limite, para nunca travar o app) */
  for (int guard = 0; guard < 20; guard++) {
    OrbisVideoOutFlipStatus st;
    memset(&st, 0, sizeof(st));
    if (sceVideoOutGetFlipStatus(video, &st) != 0) break;
    if (st.flipArg == frame_id) break;
    OrbisKernelEvent ev;
    int cnt = 0;
    OrbisKernelUseconds to = 50000;
    if (sceKernelWaitEqueue(flipq, &ev, 1, &cnt, &to) != 0) break;
  }
  cur_buf = (cur_buf + 1) % NBUF;
}

/* ------------------------------------------------------------------ controle */
static int pad = -1;
static int user_id = -1;

static void pad_open(void) {
  static int pad_inited = 0, fails = 0;
  char m[96];

  /* scePadInit() e obrigatorio ANTES de scePadOpen (sem ele o controle nunca abre). */
  if (!pad_inited) {
    int r = scePadInit();
    snprintf(m, sizeof(m), "scePadInit rc=0x%x", (unsigned)r);
    install_log(m);
    pad_inited = 1;
  }
  if (user_id < 0) {
    OrbisUserServiceInitializeParams p;
    p.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&p);
    if (sceUserServiceGetInitialUser(&user_id) < 0) user_id = -1;
    snprintf(m, sizeof(m), "usuario inicial=%d", user_id);
    install_log(m);
  }
  if (user_id >= 0) {
    pad = scePadOpen(user_id, 0, 0, NULL);
    if (pad >= 0) {
      snprintf(m, sizeof(m), "Controle aberto (handle=%d).", pad);
      install_log(m);
    } else if (fails++ < 3) {                      /* nao enche o log a cada tentativa */
      snprintf(m, sizeof(m), "AVISO: scePadOpen falhou rc=0x%x", (unsigned)pad);
      install_log(m);
    }
  }
}

/* Estado de repeticao: setas repetem ao segurar; X/O/L1/R1 so disparam ao apertar. */
#define NKEYS 6
static const struct { uint32_t mask; int key; int repeat; } KEYMAP[NKEYS] = {
  { ORBIS_PAD_BUTTON_UP,     UI_UP,    1 },
  { ORBIS_PAD_BUTTON_DOWN,   UI_DOWN,  1 },
  { ORBIS_PAD_BUTTON_LEFT,   UI_LEFT,  1 },
  { ORBIS_PAD_BUTTON_RIGHT,  UI_RIGHT, 1 },
  { ORBIS_PAD_BUTTON_L1,     UI_L1,    0 },
  { ORBIS_PAD_BUTTON_R1,     UI_R1,    0 },
};

static void poll_input(UI *ui) {
  static uint32_t prev = 0;
  static int hold[32];
  static int retry = 0;

  if (pad < 0) {                                   /* sem controle: tenta de novo a cada ~1 s */
    if (++retry >= 60) { retry = 0; pad_open(); }
    return;
  }
  OrbisPadData d;
  memset(&d, 0, sizeof(d));
  /* So erro negativo conta como falha (o exemplo oficial ignora o retorno). */
  if (scePadReadState(pad, &d) < 0) return;

  uint32_t b = d.buttons;
  static int logged = 0;
  if (b && logged < 3) {                          /* prova nos logs que o controle esta sendo lido */
    char m[64];
    snprintf(m, sizeof(m), "Botoes lidos: 0x%x", (unsigned)b);
    install_log(m);
    logged++;
  }
  /* analogico esquerdo tambem navega (centro = 0x80) */
  if (d.leftStick.x < 0x40) b |= ORBIS_PAD_BUTTON_LEFT;
  if (d.leftStick.x > 0xC0) b |= ORBIS_PAD_BUTTON_RIGHT;
  if (d.leftStick.y < 0x40) b |= ORBIS_PAD_BUTTON_UP;
  if (d.leftStick.y > 0xC0) b |= ORBIS_PAD_BUTTON_DOWN;

  uint32_t pressed = b & ~prev;

  for (int i = 0; i < NKEYS; i++) {
    uint32_t m = KEYMAP[i].mask;
    if (pressed & m) {
      ui_key(ui, KEYMAP[i].key);
      hold[i] = 0;
    } else if ((b & m) && KEYMAP[i].repeat) {
      hold[i]++;
      if (hold[i] > 24 && (hold[i] - 24) % 6 == 0) ui_key(ui, KEYMAP[i].key);   /* ~0,4 s e depois ~10/s */
    } else {
      hold[i] = 0;
    }
  }
  if (pressed & ORBIS_PAD_BUTTON_CROSS)  ui_key(ui, UI_OK);
  if (pressed & ORBIS_PAD_BUTTON_CIRCLE) ui_key(ui, UI_BACK);
  prev = b;
}

/* ------------------------------------------------------------------ instalacao (thread) */
typedef struct { CatButton btn; } Job;

static pthread_mutex_t res_mutex = PTHREAD_MUTEX_INITIALIZER;
static int res_ready = 0, res_code = 0;
static char res_err[200];

static void *install_thread(void *arg) {
  Job *j = (Job *)arg;
  char err[200] = "";
  int rc = install_pkg(j->btn.url, j->btn.name, j->btn.content_id, j->btn.type, j->btn.size, err, sizeof(err));
  pthread_mutex_lock(&res_mutex);
  res_code = rc;
  snprintf(res_err, sizeof(res_err), "%s", err);
  res_ready = 1;
  pthread_mutex_unlock(&res_mutex);
  free(j);
  return NULL;
}

static void on_install(const CatButton *b, void *user) {
  (void)user;
  Job *j = (Job *)malloc(sizeof(Job));
  if (!j) {
    pthread_mutex_lock(&res_mutex);
    res_code = -9; snprintf(res_err, sizeof(res_err), "Sem memoria."); res_ready = 1;
    pthread_mutex_unlock(&res_mutex);
    return;
  }
  j->btn = *b;
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 256 * 1024);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&t, &at, install_thread, j) != 0) {
    pthread_mutex_lock(&res_mutex);
    res_code = -9; snprintf(res_err, sizeof(res_err), "Nao foi possivel iniciar a instalacao."); res_ready = 1;
    pthread_mutex_unlock(&res_mutex);
    free(j);
  }
  pthread_attr_destroy(&at);
}

static void check_install_result(UI *ui) {
  int ready, code;
  char err[200];
  pthread_mutex_lock(&res_mutex);
  ready = res_ready; code = res_code;
  snprintf(err, sizeof(err), "%s", res_err);
  res_ready = 0;
  pthread_mutex_unlock(&res_mutex);
  if (!ready) return;
  ui_set_busy(ui, 0);
  if (code == 0) ui_toast(ui, "Pedido aceito! Acompanhe nas Notificações do PS4.", UI_TOAST_OK);
  else ui_toast(ui, err[0] ? err : "Falha ao enviar o pedido.", UI_TOAST_ERR);
}

/* ------------------------------------------------------------------ main */
int main(void) {
  install_log("=== PS4 PATCH STORE (nativo) ===");
  sceSystemServiceHideSplashScreen();

  if (video_init() != 0) return 1;
  Image *screen = img_new(FW, FH);
  if (!screen) { install_log("ERRO: sem memoria para a tela"); return 1; }

  /* mostra algo logo, enquanto carrega fontes e imagens */
  gfx_fill(screen, 0, 0, FW, FH, RGB(0x08, 0x0d, 0x18), 255);
  present(screen);
  install_log("Video ok.");

  static Catalog cat;
  if (catalog_load(&cat, CATALOG) != 0) {
    install_log("ERRO: catalog.dat ausente ou vazio");
    gfx_fill(screen, 0, 0, FW, FH, RGB(0x40, 0x10, 0x10), 255);
    present(screen);
    for (;;) sceKernelUsleep(1000000);
  }
  install_log("Catalogo carregado.");

  install_start_server();
  pad_open();
  if (pad < 0) install_log("AVISO: controle nao abriu; tentando de novo.");

  UI *ui = ui_create(screen, &cat, ASSETS, on_install, NULL);
  if (!ui) { install_log("ERRO: ui_create falhou"); return 1; }
  install_log("UI pronta.");

  ui_draw(ui);
  present(screen);
  ui_take_dirty(ui);

  for (;;) {
    poll_input(ui);
    check_install_result(ui);
    ui_tick(ui);
    if (ui_take_dirty(ui)) {
      ui_draw(ui);
      present(screen);
    }
    sceKernelUsleep(TICK_US);
  }
  return 0;
}
