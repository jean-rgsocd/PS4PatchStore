/* eboot.bin (NAO testado em hardware) - OpenOrbis Toolchain.
 * Serve /app0/www em 127.0.0.1:8080 e abre o navegador do sistema nessa URL.
 * Cada conexao roda em uma thread propria, com timeout, para que conexoes vazias
 * (pre-conexoes do navegador) nao travem o servidor. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern int sceSystemServiceHideSplashScreen(void);                /* libSceSystemService */
#ifndef NO_LAUNCH
extern int sceSystemServiceLaunchWebBrowser(const char *uri);     /* libSceSystemService */
#endif

#define PORT 8080
#ifndef START_URL
#define START_URL "http://127.0.0.1:8080/"   /* troque por http://seu-site/ se o local falhar */
#endif
#define LOG  "/data/ps4store.log"
#define ROOT "/app0/www"

static void logf_(const char *m) {
  int f = open(LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (f >= 0) { write(f, m, strlen(m)); write(f, "\n", 1); close(f); }
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

static void send_all(int c, const char *p, int n) {
  while (n > 0) {
    int w = write(c, p, n);
    if (w <= 0) return;
    p += w; n -= w;
  }
}

static void serve(int c) {
  char req[2048], path[256], full[300], buf[4096], hdr[256], msg[320];
  int n = read(c, req, sizeof(req) - 1);
  if (n <= 0) return;
  req[n] = 0;
  if (sscanf(req, "GET %255s", path) != 1 || strstr(path, "..")) {
    const char *r = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send_all(c, r, (int)strlen(r));
    return;
  }
  char *q = strchr(path, '?'); if (q) *q = 0;
  if (!strcmp(path, "/")) strcpy(path, "/index.html");
  snprintf(full, sizeof(full), "%s%s", ROOT, path);
  int fd = open(full, O_RDONLY);
  snprintf(msg, sizeof(msg), "GET %s -> %s", path, fd < 0 ? "404" : "200");
  logf_(msg);
  if (fd < 0) {
    const char *r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send_all(c, r, (int)strlen(r));
    return;
  }
  snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", mime(full));
  send_all(c, hdr, (int)strlen(hdr));
  while ((n = read(fd, buf, sizeof(buf))) > 0) send_all(c, buf, n);
  close(fd);
}

static void *worker(void *arg) {
  int c = (int)(long)arg;
  struct timeval tv;
  tv.tv_sec = 3; tv.tv_usec = 0;
  setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  tv.tv_sec = 10;
  setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  serve(c);
  close(c);
  return NULL;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);          /* navegador fechando a conexao nao pode derrubar o app */
  logf_("inicio");
  sceSystemServiceHideSplashScreen();

  int s = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a; memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 16) < 0) {
    logf_("ERRO: bind/listen falhou");
    return 1;
  }
  logf_("servidor ok, abrindo navegador");
#ifndef NO_LAUNCH
  if (sceSystemServiceLaunchWebBrowser(START_URL) != 0) logf_("ERRO: LaunchWebBrowser falhou");
#else
  logf_("modo teste: sem abrir navegador");
#endif
  for (;;) {
    int c = accept(s, NULL, NULL);
    if (c < 0) continue;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, worker, (void *)(long)c) != 0) {
      logf_("AVISO: pthread_create falhou, atendendo na thread principal");
      worker((void *)(long)c);
    }
    pthread_attr_destroy(&at);
  }
  return 0;
}
