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

extern int sceSystemServiceHideSplashScreen(void);
#ifndef NO_LAUNCH
extern int sceSystemServiceLaunchWebBrowser(const char *uri);
#endif

#define PORT 8080
#define LOG  "/data/ps4store.log"
#define ROOT "/app0/www"
#define PAYLOAD_PATH "/app0/directpayload.bin"

static void logf_(const char *m) {
  int f = open(LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (f >= 0) { 
    write(f, m, strlen(m)); 
    write(f, "\n", 1); 
    close(f); 
  }
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

// Função que injeta o payload do DirectPackageInstaller no BinLoader do GoldHEN para acionar o BGFT
static int trigger_bgft_install(const char *pkg_url) {
  logf_("Iniciando despacho do payload para o BinLoader BGFT...");

  int fd = open(PAYLOAD_PATH, O_RDONLY);
  if (fd < 0) {
    logf_("ERRO: Nao foi possivel abrir o directpayload.bin em /app0/");
    return -1;
  }

  int size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  char *payload_data = malloc(size);
  if (!payload_data) {
    close(fd);
    return -1;
  }
  read(fd, payload_data, size);
  close(fd);

  // Tenta conectar nas portas comuns do BinLoader do GoldHEN (9090, 9021, 9020)
  int sock = -1;
  int ports[] = {9090, 9021, 9020};
  for (int p = 0; p < 3; p++) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) continue;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ports[p]);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
      sock = s;
      break;
    }
    close(s);
  }

  if (sock < 0) {
    logf_("ERRO: Falha ao conectar no BinLoader do GoldHEN (Ative o Hen / Verifique a porta).");
    free(payload_data);
    return -1;
  }

  send_all(sock, payload_data, size);
  close(sock);
  free(payload_data);
  logf_("Payload despachado com sucesso para o BGFT!");
  return 0;
}

static void serve(int c) {
  char req[4096], path[512], full[600], buf[4096], hdr[256];
  int n = read(c, req, sizeof(req) - 1);
  if (n <= 0) return;
  req[n] = 0;

  // Tratar rota de instalação via GET parameter e disparar o BGFT
  if (!strncmp(req, "GET /api/install", 16)) {
    logf_("API /api/install (GET) acionada!");
    char url[512] = {0};
    
    char *u_param = strstr(req, "url=");
    if (u_param) {
      u_param += 4;
      int i = 0;
      while (u_param[i] != '\0' && u_param[i] != ' ' && u_param[i] != '\r' && u_param[i] != '\n' && i < 511) {
        url[i] = u_param[i];
        i++;
      }
      url[i] = '\0';
    }

    if (strlen(url) == 0) {
      logf_("ERRO: URL parameter vazio");
      const char *resp = "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      send_all(c, resp, (int)strlen(resp));
      return;
    }

    logf_("URL capturada com sucesso via GET:");
    logf_(url);

    // Dispara o payload nativo para o BGFT
    int res = trigger_bgft_install(url);

    const char *resp;
    if (res == 0) {
      resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"success\"}";
    } else {
      resp = "HTTP/1.1 500 Internal Server Error\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"Falha no BinLoader\"}";
    }
    send_all(c, resp, (int)strlen(resp));
    return;
  }

  if (sscanf(req, "GET %255s", path) != 1 || strstr(path, "..")) {
    const char *r = "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send_all(c, r, (int)strlen(r));
    return;
  }
  
  char *q = strchr(path, '?'); if (q) *q = 0;
  if (!strcmp(path, "/")) strcpy(path, "/index.html");
  snprintf(full, sizeof(full), "%s%s", ROOT, path);
  int fd = open(full, O_RDONLY);
  if (fd < 0) {
    const char *r = "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send_all(c, r, (int)strlen(r));
    return;
  }
  
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
  tv.tv_sec = 10;
  setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  serve(c);
  close(c);
  return NULL;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  logf_("=== INICIO DO APP (MODO GET + BGFT) ===");
  sceSystemServiceHideSplashScreen();

  int s = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a; memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = inet_addr("127.0.0.1");
  
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 16) < 0) {
    logf_("ERRO FATAL: bind/listen falhou");
    return 1;
  }
  
  logf_("Servidor HTTP rodando na porta 8080!");
#ifndef NO_LAUNCH
  if (sceSystemServiceLaunchWebBrowser("http://127.0.0.1:8080/") != 0) {
    logf_("AVISO: Falha ao abrir navegador");
  }
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
      worker((void *)(long)c);
    }
    pthread_attr_destroy(&at);
  }
  return 0;
}
