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

#define PORT 8080
#define LOG  "/data/ps4store.log"
#define ROOT "/app0/www"

static void logf_(const char *m) {
  int f = open(LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (f >= 0) { 
    write(f, m, strlen(m)); 
    write(f, "\n", 1); 
    close(f); 
  }
}

// Função para descodificar URLs (URL decode)
static void url_decode(char *dst, const char *src) {
  char a, b;
  while (*src) {
    if ((*src == '%') && (src[1] && src[2]) && (isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2]))) {
      a = src[1];
      b = src[2];
      if (a >= 'a') a = a - 'a' + 10;
      else if (a >= 'A') a = a - 'A' + 10;
      else a = a - '0';
      if (b >= 'a') b = b - 'a' + 10;
      else if (b >= 'A') b = b - 'A' + 10;
      else b = b - '0';
      *dst++ = (char)(16 * a + b);
      src += 3;
    } else if (*src == '+') {
      *dst++ = ' ';
      src++;
    } else {
      *dst++ = *src++;
    }
  }
  *dst = '\0';
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
  char req[4096], path[512], full[600], buf[4096], hdr[256];
  int n = read(c, req, sizeof(req) - 1);
  if (n <= 0) return;
  req[n] = 0;

  if (!strncmp(req, "GET /api/install", 16)) {
    logf_("API /api/install (GET) acionada!");
    char raw_url[512] = {0};
    char decoded_url[512] = {0};
    
    char *u_param = strstr(req, "url=");
    if (u_param) {
      u_param += 4;
      int i = 0;
      while (u_param[i] != '\0' && u_param[i] != ' ' && u_param[i] != '&' && u_param[i] != '\r' && u_param[i] != '\n' && i < 511) {
        raw_url[i] = u_param[i];
        i++;
      }
      raw_url[i] = '\0';
    }

    if (strlen(raw_url) == 0) {
      logf_("ERRO: URL parameter vazio");
      const char *resp = "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      send_all(c, resp, (int)strlen(resp));
      return;
    }

    url_decode(decoded_url, raw_url);

    logf_("URL pronta para instalacao nativa:");
    logf_(decoded_url);

    // Responde com sucesso para o app web
    const char *resp = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"success\"}";
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
  logf_("=== INICIO DO APP (SERVIDOR PURO) ===");
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
