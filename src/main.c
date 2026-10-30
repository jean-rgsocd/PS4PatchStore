/* PS4 Patch Store - eboot (OpenOrbis).
 * - Serve /app0/www em 127.0.0.1:8080 e abre o navegador do sistema.
 * - /api/install?url=..&name=..&content_id=..&size=..  -> instala via payload do DirectPackageInstaller:
 *     1) envia o payload ao BinLoader do GoldHEN (porta 9090), com IP/porta de retorno gravados no marcador B4;
 *     2) o payload conecta em 127.0.0.1:9091 e espera;
 *     3) enviamos o pacote no formato binario que o payload le (get_pkg_info). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <ctype.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern int sceSystemServiceHideSplashScreen(void);
#ifndef NO_LAUNCH
extern int sceSystemServiceLaunchWebBrowser(const char *uri);
#endif

#define PORT          8080
#define CALLBACK_PORT 9091
#define BINLOADER     9090
#define LOG           "/data/ps4store.log"
#define ROOT          "/app0/www"
#define PAYLOAD_PATH  "/app0/directpayload.bin"

static void logf_(const char *m) {
  int f = open(LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (f >= 0) { write(f, m, strlen(m)); write(f, "\n", 1); close(f); }
}

static void send_all(int c, const char *p, int n) {
  while (n > 0) {
    int w = write(c, p, n);
    if (w <= 0) return;
    p += w; n -= w;
  }
}

static int send_all_ok(int c, const char *p, int n) {
  while (n > 0) {
    int w = write(c, p, n);
    if (w <= 0) return -1;
    p += w; n -= w;
  }
  return 0;
}

/* decodifica %XX e + com limite de tamanho */
static void url_decode(char *dst, int cap, const char *src) {
  int o = 0;
  while (*src && o < cap - 1) {
    if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
      char a = src[1], b = src[2];
      a = (a >= 'a') ? a - 'a' + 10 : (a >= 'A') ? a - 'A' + 10 : a - '0';
      b = (b >= 'a') ? b - 'a' + 10 : (b >= 'A') ? b - 'A' + 10 : b - '0';
      dst[o++] = (char)(16 * a + b);
      src += 3;
    } else if (*src == '+') {
      dst[o++] = ' '; src++;
    } else {
      dst[o++] = *src++;
    }
  }
  dst[o] = 0;
}

static int get_param(const char *line, const char *key, char *out, int cap) {
  char pat[48], raw[1400];
  int i = 0;
  out[0] = 0;
  snprintf(pat, sizeof(pat), "?%s=", key);
  const char *p = strstr(line, pat);
  if (!p) { snprintf(pat, sizeof(pat), "&%s=", key); p = strstr(line, pat); }
  if (!p) return 0;
  p += strlen(pat);
  while (p[i] && p[i] != '&' && p[i] != ' ' && i < (int)sizeof(raw) - 1) { raw[i] = p[i]; i++; }
  raw[i] = 0;
  url_decode(out, cap, raw);
  return 1;
}

static const char *mime(const char *p) {
  const char *e = strrchr(p, '.');
  if (!e) return "application/octet-stream";
  if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
  if (!strcmp(e, ".css"))  return "text/css; charset=utf-8";
  if (!strcmp(e, ".js"))   return "application/javascript; charset=utf-8";
  if (!strcmp(e, ".json")) return "application/json; charset=utf-8";
  if (!strcmp(e, ".png"))  return "image/png";
  if (!strcmp(e, ".jpg") || !strcmp(e, ".jpeg")) return "image/jpeg";
  if (!strcmp(e, ".webp")) return "image/webp";
  if (!strcmp(e, ".svg"))  return "image/svg+xml";
  if (!strcmp(e, ".ico"))  return "image/x-icon";
  return "application/octet-stream";
}

/* ---------- conexao de retorno do payload ---------- */
static int waiting_sock = -1;
static pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *callback_server_thread(void *arg) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return NULL;
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(CALLBACK_PORT);
  a.sin_addr.s_addr = inet_addr("127.0.0.1");

  if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 4) < 0) {
    logf_("ERRO: porta 9091 indisponivel");
    close(s);
    return NULL;
  }
  logf_("Servidor de retorno ativo na porta 9091.");

  for (;;) {
    int c = accept(s, NULL, NULL);
    if (c < 0) continue;
    pthread_mutex_lock(&sock_mutex);
    if (waiting_sock >= 0) close(waiting_sock);
    waiting_sock = c;               /* payload esta bloqueado lendo; nao enviamos nada agora */
    pthread_mutex_unlock(&sock_mutex);
    logf_("Payload conectou e aguarda pacote.");
  }
  return NULL;
}

/* ---------- formato binario lido por get_pkg_info() do payload ----------
 * u32 1 | (u32 len + bytes) url, nome, content_id, tipo | u64 tamanho | u32 len_icone (0) */
static void put_u32(unsigned char *b, int *o, unsigned int v) { memcpy(b + *o, &v, 4); *o += 4; }
static void put_u64(unsigned char *b, int *o, unsigned long long v) { memcpy(b + *o, &v, 8); *o += 8; }
static void put_str(unsigned char *b, int *o, const char *s, int maxlen) {
  int l = (int)strlen(s);
  if (l > maxlen) l = maxlen;
  put_u32(b, o, (unsigned int)l);
  memcpy(b + *o, s, l);
  *o += l;
}

static int deliver_job(const unsigned char *buf, int len) {
  pthread_mutex_lock(&sock_mutex);
  int s = waiting_sock;
  waiting_sock = -1;
  pthread_mutex_unlock(&sock_mutex);
  if (s < 0) return -1;
  int r = send_all_ok(s, (const char *)buf, len);
  close(s);
  return r == 0 ? 0 : -2;
}

/* envia directpayload.bin ao BinLoader com IP/porta de retorno gravados no marcador B4 */
static int launch_payload(void) {
  int fd = open(PAYLOAD_PATH, O_RDONLY);
  if (fd < 0) { logf_("ERRO: /app0/directpayload.bin nao encontrado"); return -1; }

  int size = (int)lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  unsigned char *pl = malloc(size);
  if (!pl) { close(fd); return -1; }
  int got = 0, r;
  while (got < size && (r = read(fd, pl + got, size - got)) > 0) got += r;
  close(fd);

  unsigned char marker[6] = {0xb4, 0xb4, 0xb4, 0xb4, 0xb4, 0xb4};
  int found = -1;
  for (int i = 0; i + 6 <= size; i++) {
    if (!memcmp(pl + i, marker, 6)) { found = i; break; }
  }
  if (found < 0) { logf_("ERRO: marcador B4 nao encontrado no payload"); free(pl); return -1; }

  pl[found + 0] = 127; pl[found + 1] = 0; pl[found + 2] = 0; pl[found + 3] = 1;
  pl[found + 4] = (CALLBACK_PORT >> 8) & 0xFF;
  pl[found + 5] = CALLBACK_PORT & 0xFF;

  int sk = socket(AF_INET, SOCK_STREAM, 0);
  int ok = -1;
  if (sk >= 0) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(BINLOADER);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sk, (struct sockaddr *)&a, sizeof(a)) == 0) {
      ok = send_all_ok(sk, (const char *)pl, size);
    } else {
      logf_("ERRO: BinLoader (9090) recusou conexao. Ative o BinLoader no GoldHEN.");
    }
    close(sk);
  }
  free(pl);
  if (ok == 0) logf_("Payload enviado ao BinLoader (9090).");
  return ok;
}

static int install_pkg(const char *url, const char *name, const char *id,
                       unsigned long long size, char *err, int errsz) {
  unsigned char buf[4096];
  int o = 0;
  put_u32(buf, &o, 1u);
  put_str(buf, &o, url, 2000);
  put_str(buf, &o, name, 200);
  put_str(buf, &o, id, 47);
  put_str(buf, &o, "PS4GD", 15);
  put_u64(buf, &o, size);
  put_u32(buf, &o, 0u);            /* sem icone */

  if (deliver_job(buf, o) == 0) { logf_("Pacote entregue ao payload ja ativo."); return 0; }

  if (launch_payload() != 0) {
    snprintf(err, errsz, "payload nao enviado (BinLoader do GoldHEN ligado?)");
    return -1;
  }
  for (int i = 0; i < 30; i++) {      /* ate 15 s */
    usleep(500000);
    if (deliver_job(buf, o) == 0) { logf_("Pacote entregue ao payload."); return 0; }
  }
  snprintf(err, errsz, "payload nao conectou em 15s");
  logf_("ERRO: payload nao conectou em 15s");
  return -2;
}

static void respond(int c, int code, const char *msg) {
  char out[600];
  snprintf(out, sizeof(out),
    "HTTP/1.1 %d %s\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n%s",
    code, code == 200 ? "OK" : "Error", msg);
  send_all(c, out, (int)strlen(out));
}

static void serve(int c) {
  char req[4096], path[512], full[600], buf[4096], hdr[256];
  int n = read(c, req, sizeof(req) - 1);
  if (n <= 0) return;
  req[n] = 0;

  if (!strncmp(req, "GET /api/install", 16)) {
    char line[2400];
    int li = 0;
    while (req[li] && req[li] != '\r' && req[li] != '\n' && li < (int)sizeof(line) - 1) { line[li] = req[li]; li++; }
    line[li] = 0;

    char url[1400], name[300], id[80], size_s[40], err[200];
    get_param(line, "url", url, sizeof(url));
    get_param(line, "name", name, sizeof(name));
    get_param(line, "content_id", id, sizeof(id));
    get_param(line, "size", size_s, sizeof(size_s));
    unsigned long long size = strtoull(size_s, NULL, 10);

    logf_("API /api/install acionada.");
    if (!url[0])  { respond(c, 400, "URL vazia."); return; }
    if (!id[0])   { logf_("ERRO: content_id vazio"); respond(c, 400, "Preencha data-content-id deste botao (ID do PKG)."); return; }
    if (!size)    { logf_("AVISO: size = 0"); }
    if (!name[0]) strcpy(name, "PS4 Package");

    logf_(url);
    if (install_pkg(url, name, id, size, err, sizeof(err)) == 0) {
      respond(c, 200, "OK");
    } else {
      respond(c, 502, err);
    }
    return;
  }

  if (sscanf(req, "GET %511s", path) != 1 || strstr(path, "..")) {
    respond(c, 400, "");
    return;
  }

  char *q = strchr(path, '?'); if (q) *q = 0;
  if (!strcmp(path, "/")) strcpy(path, "/index.html");
  snprintf(full, sizeof(full), "%s%s", ROOT, path);
  int fd = open(full, O_RDONLY);
  if (fd < 0) { respond(c, 404, ""); return; }

  snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", mime(full));
  send_all(c, hdr, (int)strlen(hdr));
  while ((n = read(fd, buf, sizeof(buf))) > 0) send_all(c, buf, n);
  close(fd);
}

static void *worker(void *arg) {
  int c = (int)(long)arg;
  struct timeval tv;
  tv.tv_sec = 3; tv.tv_usec = 0;
  setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  tv.tv_sec = 30;
  setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  serve(c);
  close(c);
  return NULL;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  logf_("=== PS4 PATCH STORE - protocolo DPI ===");
  sceSystemServiceHideSplashScreen();

  pthread_t cb;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&cb, &attr, callback_server_thread, NULL);
  pthread_attr_destroy(&attr);

  int s = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(PORT);
  a.sin_addr.s_addr = inet_addr("127.0.0.1");

  if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 16) < 0) {
    logf_("ERRO: porta 8080 indisponivel");
    return 1;
  }
  logf_("Servidor HTTP na porta 8080.");

#ifndef NO_LAUNCH
  sceSystemServiceLaunchWebBrowser("http://127.0.0.1:8080/");
#endif

  for (;;) {
    int c = accept(s, NULL, NULL);
    if (c < 0) continue;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, worker, (void *)(long)c) != 0) worker((void *)(long)c);
    pthread_attr_destroy(&at);
  }
  return 0;
}
