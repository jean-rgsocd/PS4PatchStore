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
#include <fcntl.h>
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

#ifndef JK_DIR
#define JK_DIR "/data/jk"
#endif

/* Heap do libc: a UI usa ~60 MB (tela, fundo, capas, JPEG decodificado). */
#ifndef JK_MODULE
uint64_t sceLibcHeapSize = 256ull * 1024 * 1024;   /* no modo launcher+modulo, quem define e o launcher */
#endif

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
 * O PS4 nao instala um app por cima de outro. O app baixa o PKG novo em /data/pkg (acao "appupdate") e a tela
 * mostra o passo a passo: apagar o app e instalar pelo Package Installer do GoldHEN. O login fica em /data. */
#define OWN_ID_PREFIX "IV0000-PSTR00002"

/* Tamanho de arquivo com open/lseek (o mesmo jeito que o resto do app ja usa); -1 se nao abrir. */
static long long file_size_of(const char *p) {
  int fd = open(p, O_RDONLY);
  if (fd < 0) return -1;
  long long n = (long long)lseek(fd, 0, SEEK_END);
  close(fd);
  return n;
}

/* Copia um arquivo com open/read/write. Retorna 0 se copiou tudo. */
static int copy_file(const char *src, const char *dst) {
  int in = open(src, O_RDONLY);
  if (in < 0) return -1;
  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (out < 0) { close(in); return -1; }
  char *buf = (char *)malloc(65536);
  int ok = buf ? 0 : -1;
  while (ok == 0) {
    int n = (int)read(in, buf, 65536);
    if (n < 0) { ok = -1; break; }
    if (n == 0) break;
    for (int off = 0; off < n;) {
      int w = (int)write(out, buf + off, (size_t)(n - off));
      if (w <= 0) { ok = -1; break; }
      off += w;
    }
  }
  free(buf);
  close(in);
  close(out);
  return ok;
}

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
  if (!strcmp(action, "appupdate")) {                       /* atualizacao do app: baixa o PKG novo em /data/pkg (o GoldHEN lista la) */
    char path[512], part[520], filename[96], lm[700];
    static unsigned long long app_dl_size = 0;               /* tamanho do ultimo PKG baixado nesta abertura */
    ui_app_pkg_name(j->btn.url, filename, sizeof(filename));
    mkdir("/data/pkg", 0777);
    snprintf(path, sizeof(path), "/data/pkg/%s", filename);
    snprintf(part, sizeof(part), "%s.part", path);        /* baixa para .part e so depois copia para .pkg (o Package Installer nao ve arquivo pela metade) */
    long long have = file_size_of(path);
    if (have > 0 && ((j->btn.size > 0 && (unsigned long long)have == j->btn.size) || (app_dl_size && (unsigned long long)have == app_dl_size))) {
      snprintf(lm, sizeof(lm), "update do app: %s ja esta baixado (%lld bytes)", path, have);
      install_log(lm);
      rc = 2;
    } else {
      volatile int cancel = 0;
      snprintf(lm, sizeof(lm), "update do app: baixando %s -> %s", j->btn.url, path);
      install_log(lm);
      snprintf(dl_name, sizeof(dl_name), "%s", filename);
      dl_pct = 0;
      dl_active = 1;
      rc = auth_download(j->btn.url, part, &dl_pct, &cancel, err, sizeof(err));   /* ja confere com o tamanho que o servidor informou */
      dl_active = 0;
      long long got = (rc == 0) ? file_size_of(part) : -1;
      snprintf(lm, sizeof(lm), "update do app: download rc=%d, arquivo=%lld bytes, catalogo=%llu bytes", rc, got, j->btn.size);
      install_log(lm);
      if (rc == 0 && got > 0) {
        if (j->btn.size > 0 && (unsigned long long)got != j->btn.size)
          install_log("AVISO: tamanho do catalogo difere do PKG no R2 (o catalogo e o PKG sao do mesmo build?)");
        unlink(path);
        if (copy_file(part, path) == 0) {
          app_dl_size = (unsigned long long)got;
          install_log("update do app: PKG salvo em /data/pkg.");
          rc = 2;
        } else {
          unlink(path);
          snprintf(err, sizeof(err), "Não foi possível salvar o arquivo em /data/pkg.");
          install_log("update do app: copia para /data/pkg falhou (espaco/permissao?)");
          rc = -1;
        }
        unlink(part);
      } else {
        unlink(part);
        if (rc == 0) { snprintf(err, sizeof(err), "Download da atualização vazio. Tente de novo."); install_log("update do app: arquivo vazio"); }
        else if (!err[0]) snprintf(err, sizeof(err), "Falha ao baixar a atualização do app.");
        rc = -1;
      }
    }
  } else if (!strcmp(action, "download")) {
    const char *dir = "/data/PS4PatchStore/downloads";
    const char *d = j->btn.dest;
    if (!strncmp(d, "/data/", 6) && !strstr(d, "..") && strlen(d) < 120) dir = d;      /* destino escolhido no catalogo */
    mkdir("/data/PS4PatchStore", 0777);
    /* Lista de arquivos do botao: o principal (url/dest) + os "extras" separados por '|'.
     * Cada extra pode ser "https://..." (mesma pasta do principal) ou "/data/pasta=https://..." (outra pasta).
     * Ex.: plugin = .cpk + DpFileList.bin; Comemoracoes = 5 arquivos em 3 pastas. */
    enum { MAXIT = 33 };   /* 1 principal + ate 32 extras */
    char it_url[MAXIT][1024];
    char it_dir[MAXIT][120];
    char it_soft[MAXIT];                                /* 1 = falha nao derruba a instalacao (ex.: icone) */
    int nit = 0;
    snprintf(it_url[0], sizeof(it_url[0]), "%s", j->btn.url);
    snprintf(it_dir[0], sizeof(it_dir[0]), "%s", dir);
    it_soft[0] = 0;
    nit = 1;
    {
      char ex[8192];
      snprintf(ex, sizeof(ex), "%s", j->btn.extra);
      char *sv = ex;
      while (*sv && nit < MAXIT) {
        char *tok = sv;
        char *bar = strchr(sv, '|');
        if (bar) { *bar = 0; sv = bar + 1; } else sv += strlen(sv);
        if (!tok[0]) continue;
        const char *tdir = dir;
        char *u = tok;
        int soft = 0;
        if (!strncmp(tok, "/data/", 6) || !strncmp(tok, "/user/appmeta/", 14)) {
          char *eq = strchr(tok, '=');
          if (!eq) continue;
          *eq = 0;
          u = eq + 1;
          if (!strstr(tok, "..") && strlen(tok) < 120) {
            tdir = tok;
            if (!strncmp(tok, "/user/appmeta/", 14)) soft = 1;   /* icone do jogo: se nao puder gravar, segue sem erro */
          }
        }
        if (strncmp(u, "https://", 8) != 0) continue;
        snprintf(it_url[nit], sizeof(it_url[0]), "%s", u);
        snprintf(it_dir[nit], sizeof(it_dir[0]), "%s", tdir);
        it_soft[nit] = (char)soft;
        nit++;
      }
    }
    char lastname[180] = "";
    rc = 0;
    for (int u = 0; u < nit && rc == 0; u++) {
      char path[512], filename[180];
      {                                                /* cria a pasta e as intermediarias (mkdir -p) */
        char tmp[160];
        snprintf(tmp, sizeof(tmp), "%s", it_dir[u]);
        for (char *p = tmp + 1; *p; p++)
          if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
        mkdir(tmp, 0777);
      }
      const char *base = strrchr(it_url[u], '/');
      base = base ? base + 1 : it_url[u];
      size_t k = 0;
      while (*base && *base != '?' && k + 1 < sizeof(filename)) {
        unsigned char ch = (unsigned char)*base++;
        if (ch == '%' && base[0] && base[1]) {
          char hex[3] = { base[0], base[1], 0 };
          char *end = NULL; long v = strtol(hex, &end, 16);
          if (end && *end == 0) { ch = (unsigned char)v; base += 2; }
        }
        /* mantem o nome ORIGINAL (com espacos e acentos); so troca o que o sistema de arquivos nao aceita */
        int bad = (ch < 32 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|');
        filename[k++] = bad ? '_' : (char)ch;
      }
      filename[k] = 0;
      if (!filename[0] || !strcmp(filename, ".") || !strcmp(filename, "..")) snprintf(filename, sizeof(filename), "arquivo_download.bin");
      snprintf(path, sizeof(path), "%s/%s", it_dir[u], filename);
      volatile int cancel = 0;
      char lm[1200];
      snprintf(lm, sizeof(lm), "Iniciando download de arquivo pelo app (%d/%d): %s", u + 1, nit, it_url[u]);
      install_log(lm);
      snprintf(dl_name, sizeof(dl_name), "%s", filename);
      dl_pct = 0;
      dl_active = 1;
      rc = auth_download(it_url[u], path, &dl_pct, &cancel, err, sizeof(err));
      dl_active = 0;
      if (rc != 0) {
        snprintf(lm, sizeof(lm), "Download falhou (%s): %s", filename, err[0] ? err : "(sem mensagem)");
        install_log(lm);
        unlink(path);
        if (it_soft[u]) {                              /* item opcional (icone): registra e continua */
          install_log("item opcional falhou; seguindo a instalacao.");
          rc = 0;
          err[0] = 0;
        } else if (!err[0]) snprintf(err, sizeof(err), "Falha ao baixar o arquivo.");
      } else {
        install_log(path);
        snprintf(lastname, sizeof(lastname), "%s", filename);
      }
    }
    if (rc == 0) {
      if (nit > 2) snprintf(err, sizeof(err), "Download concluído: %d arquivos", nit);
      else if (nit == 2) snprintf(err, sizeof(err), "Download concluído: %s + 1 arquivo", lastname);
      else snprintf(err, sizeof(err), "Download concluído: %s", lastname);
      rc = 1; /* sucesso de download (diferente de pedido de instalacao) */
    }
  } else if (!strcmp(action, "video") || !strcmp(action, "image")) {
    snprintf(err, sizeof(err), "Este tipo de botão não deveria iniciar uma tarefa em segundo plano.");
    rc = -1;
  } else if (!strncmp(j->btn.content_id, OWN_ID_PREFIX, strlen(OWN_ID_PREFIX))) {      /* atualizacao do proprio app: instala por cima */
    install_log("update: enviando o PKG novo do app ao instalador (por cima, sem apagar).");
    rc = install_pkg(j->btn.url, j->btn.name, j->btn.content_id, j->btn.type, j->btn.size, err, sizeof(err));
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

static void on_preview(void *ud) {
  (void)ud;
  auth_start_preview();
}

static void on_register(const char *user, const char *pass, void *ud) {
  (void)ud;
  auth_start_register(user, pass);
}

static void on_reload(void *ud) {                       /* "Ja enviei / atualizar plano": le o plano de novo no servidor */
  (void)ud;
  install_log("Atualizando o plano (pedido do usuario).");
  auth_start_restore();
}

static void on_logout(void *ud) {
  (void)ud;
  auth_logout();
  install_log("Sessao encerrada pelo usuario.");
}

/* Lembrete de apoio (nao bloqueia nada). Quando o prazo de 30 dias do apoio vence, abre a tela "Apoie o app" ao entrar:
 * uma vez a cada 30 dias, ou TODA vez que abrir se o admin ligou o "lembrete fixo". Ao confirmar a doacao, o admin soma
 * +30 dias e desliga o lembrete. O ultimo lembrete fica em /data/jkpatchs_apoio.txt (segundos, hora do servidor). */
#define REMIND_FILE "/data/jkpatchs_apoio.txt"
static void support_reminder(UI *ui, const AuthEvent *ev) {
  if (!ev->user[0] || cat.locked || ev->plan_now < 1700000000L) return;
  if (ev->nag) { ui_remind_support(ui); install_log("Lembrete de apoio (fixo, ligado pelo admin)."); return; }
  if (ev->support_until < 1700000000L || ev->plan_now < ev->support_until) return;
  long last = 0;
  FILE *f = fopen(REMIND_FILE, "r");
  if (f) { if (fscanf(f, "%ld", &last) != 1) last = 0; fclose(f); }
  if (last > 0 && ev->plan_now - last < 30L * 86400L) return;
  ui_remind_support(ui);
  f = fopen(REMIND_FILE, "w");
  if (f) { fprintf(f, "%ld\n", ev->plan_now); fclose(f); }
  install_log("Lembrete de apoio mostrado (prazo de 30 dias vencido).");
}

/* ------------------------------------------------------------------ atualizacao do modulo (so no modo launcher+modulo) */
#ifdef JK_MODULE
static volatile int mod_state = 0;          /* 0 = nada, 1 = baixando, 2 = pronto (reabrir o app), 3 = falhou */
static CatMod mod_job;

static void *mod_thread(void *arg) {
  (void)arg;
  char err[160] = "", m[300];
  volatile int pct = 0, cancel = 0;
  const char *tmp = JK_DIR "/jkui.prx.dl", *fin = JK_DIR "/jkui.prx.new";
  mkdir(JK_DIR, 0777);
  unlink(tmp);
  snprintf(m, sizeof(m), "modulo: baixando v%s de %s", mod_job.version, mod_job.url); install_log(m);
  int rc = auth_download(mod_job.url, tmp, &pct, &cancel, err, sizeof(err));
  chmod(tmp, 0777);
  long long got = file_size_of(tmp);
  snprintf(m, sizeof(m), "modulo: download rc=%d, arquivo=%lld bytes, catalogo=%llu bytes", rc, got, mod_job.size); install_log(m);
  if (rc == 0 && got > 0 && (mod_job.size == 0 || (unsigned long long)got == mod_job.size)) {
    unlink(fin);
    if (rename(tmp, fin) == 0) { install_log("modulo: novo modulo pronto; sera aplicado na proxima abertura."); mod_state = 2; return NULL; }
  }
  unlink(tmp);
  install_log("modulo: atualizacao falhou (tamanho diferente ou erro de rede).");
  mod_state = 3;
  return NULL;
}

static void mod_update_check(const Catalog *c) {
  if (!c->mod.url[0] || mod_state) return;
  if (catalog_version_num(c->mod.version) <= catalog_version_num(APP_VERSION)) {
    unlink(JK_DIR "/jkui.prx.new");
    return;
  }
  long long have = file_size_of(JK_DIR "/jkui.prx.new");
  if (have > 0 && (c->mod.size == 0 || (unsigned long long)have == c->mod.size)) { mod_state = 2; return; }   /* ja baixado */
  mod_job = c->mod;
  mod_state = 1;
  pthread_t t; pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 256 * 1024);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&t, &a, mod_thread, NULL) != 0) mod_state = 3;
  pthread_attr_destroy(&a);
}

static void mod_update_poll(UI *ui) {
  static int shown = 0;
  if (mod_state == 2 && shown != 2) {
    shown = 2;
    ui_toast(ui, "Atualização do app baixada. Feche e abra o app para aplicar.", UI_TOAST_INFO);
  }
}
#else
static void mod_update_check(const Catalog *c) { (void)c; }
static void mod_update_poll(UI *ui) { (void)ui; }
#endif

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
    case AUTH_EV_CATALOG: {
      if (catalog_parse(&cat, ev.text) == 0) {
        ui_set_offline(ui, ev.offline);                       /* a copia salva (auth.c) vem marcada como offline */
        ui_set_user(ui, ev.user);
        if (ev.offline) {                                     /* servidor fora do ar: loja salva */
          ui_set_plan(ui, "SEM CONEXÃO COM O SERVIDOR  \xC2\xB7  LOJA SALVA", 1);
        } else if (!ev.user[0]) {                             /* visitante (sem login) */
          ui_set_plan(ui, "MODO VISITANTE  \xC2\xB7  DOWNLOADS BLOQUEADOS", 1);
        } else if (ev.state[0]) {                             /* servidor novo: o app calcula o texto, o aviso de 48 h e o bloqueio */
          ui_set_plans_text(ui, ev.plans);
          ui_set_plan_info(ui, ev.state, ev.plan_name, ev.expires, ev.plan_now);
        } else if (!strcmp(ev.plan, "pendente")) {
          char pt[96];
          int lv = auth_plan_text(ev.plan, ev.plan_now, pt, sizeof(pt));
          ui_set_plan(ui, lv >= 0 ? pt : "", lv >= 0 ? lv : 0);
        } else {
          ui_set_plan(ui, "CONTA ATIVA", 0);                   /* sem vencimento: o app e livre */
        }
        ui_set_catalog(ui, &cat);
        install_log("Catalogo recebido do servidor.");
        mod_update_check(&cat);
        if (ev.registered && ev.user[0]) ui_open_shop(ui);       /* conta nova: mostra os planos (o teste gratis ja esta valendo) */
        if (!ev.offline) support_reminder(ui, &ev);
        if (cat.app.url[0] && catalog_version_num(cat.app.version) <= catalog_version_num(APP_VERSION)) {
          char fn[96], pp[160];                    /* app ja esta na versao do catalogo: apaga o PKG da atualizacao (libera espaco) */
          ui_app_pkg_name(cat.app.url, fn, sizeof(fn));
          snprintf(pp, sizeof(pp), "/data/pkg/%s", fn);
          unlink(pp);
        }
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
    }
    case AUTH_EV_CATALOG_FAIL:
      /* sem copia salva no console (a copia, quando existe, ja foi usada em auth.c): mostra o motivo */
      ui_show_login(ui, ev.msg[0] ? ev.msg : "Não foi possível baixar o catálogo.");
      break;
    default:
      break;
  }
  free(ev.text);
}

/* ------------------------------------------------------------------ teste de modulo (so no build de teste: -DJK_MODTEST) */
#ifdef JK_MODTEST
extern int jk_load_start_module(const char *name, size_t argc, const void *argv, unsigned flags, void *opt, int *res) __asm__("sceKernelLoadStartModule");
extern int jk_dlsym(int handle, const char *sym, void **addr) __asm__("sceKernelDlsym");

static void modtest_one(const char *path) {
  char b[300];
  struct stat st;
  if (stat(path, &st) != 0) { snprintf(b, sizeof(b), "MODTESTE: %s nao existe", path); install_log(b); return; }
  snprintf(b, sizeof(b), "MODTESTE: %s existe (%lld bytes)", path, (long long)st.st_size); install_log(b);
  int res = 0;
  int h = jk_load_start_module(path, 0, NULL, 0, NULL, &res);
  snprintf(b, sizeof(b), "MODTESTE: LoadStartModule -> 0x%08x (res=%d)", (unsigned)h, res); install_log(b);
  if (h < 0) return;
  void *fn = NULL;
  int r = jk_dlsym(h, "jkmod_entry", &fn);
  snprintf(b, sizeof(b), "MODTESTE: Dlsym jkmod_entry -> 0x%08x ptr=%p", (unsigned)r, fn); install_log(b);
  if (r == 0 && fn) {
    int v = ((int (*)(int))fn)(41);
    snprintf(b, sizeof(b), "MODTESTE: jkmod_entry(41) = %d (esperado 42)", v); install_log(b);
  }
}

static void modtest(void) {
  install_log("MODTESTE: iniciando");
  modtest_one("/app0/jkmod_test.prx");
  modtest_one("/data/jk/jkmod_test.prx");
  install_log("MODTESTE: fim");
}
#endif

/* ------------------------------------------------------------------ main */
#ifdef JK_MODULE
int __attribute__((visibility("default"))) jkui_main(void) {
#else
int main(void) {
#endif
  install_log("=== JK PATCHS (nativo) v" APP_VERSION " ===");
#ifdef JK_CRASHFLAG                    /* so no teste do rollback: se existir crash.flag, apaga e TRAVA de proposito (uma vez) */
  int cf = open(JK_DIR "/crash.flag", O_RDONLY);   /* open(), nao access(): access nao funciona direito neste ambiente */
  if (cf >= 0) {
    close(cf);
    unlink(JK_DIR "/crash.flag");
    install_log("MODULO DE TESTE: travando de proposito.");
    __builtin_trap();            /* ud2: derruba o app sempre (escrever no endereco 0 nao derruba neste PS4) */
  }
#endif
#ifdef JK_MODTEST
  modtest();
#endif
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
  ui_set_preview_handler(ui, on_preview);
  ui_set_reload_handler(ui, on_reload);
  ui_set_open_handler(ui, on_open_url, NULL);
  ui_set_cover_fetcher(ui, on_cover_fetch, "/data/jkcovers", NULL);
  ui_show_loading(ui, "Conectando ao servidor...");
  install_log("UI pronta; verificando sessao salva.");
  auth_start_restore();

  ui_draw(ui);
  present(screen);
  ui_take_dirty(ui);
#ifdef JK_MODULE
  unlink(JK_DIR "/boot.pending");   /* subiu sem travar: o launcher pode confiar neste modulo */
#endif

  for (;;) {
    poll_input(ui);
    check_install_result(ui);
    check_download_progress(ui);
    check_auth(ui);
    mod_update_poll(ui);
    ui_tick(ui);
    if (ui_take_dirty(ui)) {
      ui_draw(ui);
      present(screen);
    }
    sceKernelUsleep(TICK_US);
  }
  return 0;
}

#ifdef JK_MODULE
const char * __attribute__((visibility("default"))) jkui_version(void) { return APP_VERSION; }
#endif
