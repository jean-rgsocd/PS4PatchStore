/* JK Patchs - app NATIVO (OpenOrbis).
 * Desenha a interface direto na tela (1920x1080, por software), le o controle e instala os pacotes
 * pelo mesmo payload do DirectPackageInstaller usado pela versao navegador.
 *
 * Arquivos esperados em /app0: server.url, directpayload.bin e as pastas assets/fonts e assets/images.
 * O catalogo vem do servidor (HTTPS) depois do login; nao fica dentro do pacote.
 * Log de diagnostico: /data/ps4store.log */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <ctype.h>

#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>
#include <orbis/AudioOut.h>
#include <orbis/Sysmodule.h>

#include "gfx.h"
#include "ui.h"
#include "catalog.h"
#include "install.h"
#include "auth.h"
#include "auth_proto.h"
#include "version.h"
#include "vplayer.h"

/* Heap do libc: a UI usa ~60 MB (tela, fundo, capas, JPEG decodificado). */
uint64_t sceLibcHeapSize = 256ull * 1024 * 1024;

#define FW        1920
#define FH        1080
#define NBUF      2
#define ASSETS    "/app0/assets"
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
  if (pressed & ORBIS_PAD_BUTTON_SQUARE) ui_key(ui, UI_SQUARE);
  if (pressed & ORBIS_PAD_BUTTON_TRIANGLE) ui_key(ui, UI_TRIANGLE);
  if (pressed & ORBIS_PAD_BUTTON_OPTIONS) ui_key(ui, UI_OPTIONS);
  prev = b;
}

/* ------------------------------------------------------------------ atualizacao do proprio app
 * O DPI recusa reinstalar o app por cima e o app nao consegue se desinstalar sozinho (o sistema nao libera o
 * instalador para apps). Por isso a atualizacao do app e manual: o botao so avisa como fazer. */
#define OWN_ID_PREFIX "IV0000-PSTR00002"

/* ------------------------------------------------------------------ instalacao (thread) */
typedef struct { CatButton btn; } Job;

/* progresso do download de arquivo (a thread escreve, o laco principal mostra na tela) */
static volatile int dl_active = 0, dl_pct = 0;
static char dl_name[120];

static pthread_mutex_t res_mutex = PTHREAD_MUTEX_INITIALIZER;
static int res_ready = 0, res_code = 0;
static char res_err[200];

static void *install_thread(void *arg) {
  Job *j = (Job *)arg;
  char err[200] = "";
  int rc;
  const char *action = j->btn.action[0] ? j->btn.action : "pkg";
  if (!strcmp(action, "download")) {
    const char *dir = "/data/PS4PatchStore/downloads";
    char path[512], filename[180];
    const char *base = strrchr(j->btn.url, '/');
    base = base ? base + 1 : j->btn.url;
    size_t k = 0;
    while (*base && *base != '?' && k + 1 < sizeof(filename)) {
      unsigned char ch = (unsigned char)*base++;
      if (ch == '%' && base[0] && base[1]) {
        char hex[3] = { base[0], base[1], 0 };
        char *end = NULL; long v = strtol(hex, &end, 16);
        if (end && *end == 0) { ch = (unsigned char)v; base += 2; }
      }
      filename[k++] = (isalnum(ch) || ch == '.' || ch == '_' || ch == '-') ? (char)ch : '_';
    }
    filename[k] = 0;
    if (!filename[0] || !strcmp(filename, ".") || !strcmp(filename, "..")) snprintf(filename, sizeof(filename), "arquivo_download.bin");
    mkdir("/data/PS4PatchStore", 0777);
    mkdir(dir, 0777);
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    volatile int cancel = 0;
    char lm[700];
    snprintf(lm, sizeof(lm), "Iniciando download de arquivo pelo app: %s", j->btn.url);
    install_log(lm);
    snprintf(dl_name, sizeof(dl_name), "%s", filename);
    dl_pct = 0;
    dl_active = 1;
    rc = auth_download(j->btn.url, path, &dl_pct, &cancel, err, sizeof(err));
    dl_active = 0;
    if (rc != 0) {
      snprintf(lm, sizeof(lm), "Download falhou: %s", err[0] ? err : "(sem mensagem)");
      install_log(lm);
    }
    if (rc == 0) {
      snprintf(err, sizeof(err), "Download concluído: %s", filename);
      install_log(path);
      rc = 1; /* sucesso de download (diferente de pedido de instalacao) */
    } else {
      unlink(path);
      if (!err[0]) snprintf(err, sizeof(err), "Falha ao baixar o arquivo.");
    }
  } else if (!strcmp(action, "video") || !strcmp(action, "image")) {
    snprintf(err, sizeof(err), "Este tipo de botão não deveria iniciar uma tarefa em segundo plano.");
    rc = -1;
  } else if (!strncmp(j->btn.content_id, OWN_ID_PREFIX, strlen(OWN_ID_PREFIX))) {      /* atualizacao do proprio app */
    install_log("update: atualizacao manual (apagar o app e instalar o PKG novo).");
    snprintf(err, sizeof(err), "Nova versão disponível. Apague o JK Patchs (Opções > Excluir) e instale a versão nova.");
    rc = -1;
    goto done;
  } else {
    rc = install_pkg(j->btn.url, j->btn.name, j->btn.content_id, j->btn.type, j->btn.size, err, sizeof(err));
  }
done:
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

static void check_download_progress(UI *ui) {
  static int last = -1;
  if (!dl_active) { last = -1; return; }
  int p = dl_pct;
  if (p == last) return;
  last = p;
  char m[220];
  snprintf(m, sizeof(m), "Baixando %s...  %d%%", dl_name, p);
  ui_toast(ui, m, UI_TOAST_INFO);
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
  ui_install_done(ui, code, err);
}

/* ------------------------------------------------------------------ video dentro do app (audio + ganchos) */
static int aud = -1;

static int vp_audio_open(void) {
  sceAudioOutInit();
  aud = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000,
                        ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_MONO);
  if (aud < 0) {
    char m[80];
    snprintf(m, sizeof(m), "AVISO: audio nao abriu (0x%x); video sem som.", (unsigned)aud);
    install_log(m);
    aud = -1;
    return -1;
  }
  return 0;
}
static void vp_audio_write(const int16_t *pcm, int n) {
  for (int i = 0; i + 256 <= n && aud >= 0; i += 256) sceAudioOutOutput(aud, pcm + i);
}
static void vp_audio_close(void) {
  if (aud >= 0) { sceAudioOutOutput(aud, NULL); sceAudioOutClose(aud); aud = -1; }
}
static void vp_sleep(unsigned us) { sceKernelUsleep(us); }

static const VpPlatform VP_PLAT = {
  auth_download, vp_audio_open, vp_audio_write, vp_audio_close, vp_sleep, install_log, "/data/jkpatchs_video.tmp"
};

/* ------------------------------------------------------------------ video (navegador do PS4) */
static void on_open_url(const char *url, void *ud) {
  (void)ud;
  if (strncmp(url, "https://", 8) != 0) return;               /* so https */
  char m[260];
  snprintf(m, sizeof(m), "Abrindo video: %s", url);
  install_log(m);
  /* mesma chamada usada pela versao navegador (uma URL); o prototipo do header e vazio */
  ((int (*)(const char *))sceSystemServiceLaunchWebBrowser)(url);
}

/* ------------------------------------------------------------------ login / catalogo do servidor */
static Catalog cat;                  /* o catalogo recebido do servidor (a UI aponta para ele) */

static void on_login(const char *user, const char *pass, void *ud) {
  (void)ud;
  auth_start_login(user, pass);
}

static int on_cover_fetch(const char *url, const char *dest, void *ud) {
  (void)ud;
  volatile int pct = 0, cancel = 0;
  char err[160];
  return auth_download(url, dest, &pct, &cancel, err, sizeof(err));
}

static void on_register(const char *user, const char *pass, void *ud) {
  (void)ud;
  auth_start_register(user, pass);
}

static void on_logout(void *ud) {
  (void)ud;
  auth_logout();
  install_log("Sessao encerrada pelo usuario.");
}

static void check_auth(UI *ui) {
  AuthEvent ev;
  if (!auth_poll(&ev)) return;
  switch (ev.type) {
    case AUTH_EV_NEED_LOGIN:
      ui_show_login(ui, ev.msg);
      break;
    case AUTH_EV_LOGIN_FAIL:
    case AUTH_EV_REGISTER_FAIL:
      ui_show_login(ui, ev.msg);
      break;
    case AUTH_EV_REGISTER_OK:
      ui_show_register_done(ui, ev.user);
      break;
    case AUTH_EV_CATALOG:
      if (catalog_parse(&cat, ev.text) == 0) {
        ui_set_user(ui, ev.user);
        {
          char pt[96];
          int lv = auth_plan_text(ev.plan, ev.plan_now, pt, sizeof(pt));
          ui_set_plan(ui, lv >= 0 ? pt : "", lv >= 0 ? lv : 0);
        }
        ui_set_catalog(ui, &cat);
        install_log("Catalogo recebido do servidor.");
        {
          char m[200];
          snprintf(m, sizeof(m), "Versao do app: instalada=%s | servidor=%s (link do PKG: %s, id: %s)", APP_VERSION,
                   cat.app.version[0] ? cat.app.version : "sem linha APP", cat.app.url[0] ? "sim" : "nao",
                   cat.app.content_id[0] ? "sim" : "nao");
          install_log(m);
        }
      } else {
        ui_show_login(ui, "O servidor enviou um catálogo vazio ou inválido.");
        install_log("ERRO: catalogo do servidor invalido");
      }
      break;
    case AUTH_EV_CATALOG_FAIL:
      /* sessao continua salva; so mostra o motivo e deixa tentar de novo pela tela de login */
      ui_show_login(ui, ev.msg[0] ? ev.msg : "Não foi possível baixar o catálogo.");
      break;
    default:
      break;
  }
  free(ev.text);
}

/* ------------------------------------------------------------------ main */
int main(void) {
  install_log("=== JK PATCHS (nativo) v" APP_VERSION " ===");
  sceSystemServiceHideSplashScreen();

  if (video_init() != 0) return 1;
  Image *screen = img_new(FW, FH);
  if (!screen) { install_log("ERRO: sem memoria para a tela"); return 1; }

  /* mostra algo logo, enquanto carrega fontes e imagens */
  gfx_fill(screen, 0, 0, FW, FH, RGB(0x08, 0x0d, 0x18), 255);
  present(screen);
  install_log("Video ok.");

  vp_init(&VP_PLAT);
  install_start_server();
  pad_open();
  if (pad < 0) install_log("AVISO: controle nao abriu; tentando de novo.");

  UI *ui = ui_create(screen, NULL, ASSETS, on_install, NULL);
  if (!ui) { install_log("ERRO: ui_create falhou"); return 1; }
  ui_set_auth_handlers(ui, on_login, on_logout, NULL);
  ui_set_register_handler(ui, on_register);
  ui_set_open_handler(ui, on_open_url, NULL);
  ui_set_cover_fetcher(ui, on_cover_fetch, "/data/jkcovers", NULL);
  ui_show_loading(ui, "Conectando ao servidor...");
  install_log("UI pronta; verificando sessao salva.");
  auth_start_restore();

  ui_draw(ui);
  present(screen);
  ui_take_dirty(ui);

  for (;;) {
    poll_input(ui);
    check_install_result(ui);
    check_download_progress(ui);
    check_auth(ui);
    ui_tick(ui);
    if (ui_take_dirty(ui)) {
      ui_draw(ui);
      present(screen);
    }
    sceKernelUsleep(TICK_US);
  }
  return 0;
}
