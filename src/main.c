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
#define PAYLOAD_PATH "/app0/directpayload.bin"

static void logf_(const char *m) {
  int f = open(LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (f >= 0) { 
    write(f, m, strlen(m)); 
    write(f, "\n", 1); 
    close(f); 
  }
}

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

// Variável global temporária para armazenar a última URL solicitada para o retorno TCP
static char current_target_url[512] = {0};
static pthread_mutex_t url_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread do Servidor de Retorno TCP que o payload no PS4 conecta de volta[cite: 5]
static void *callback_server_thread(void *arg) {
  int server_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (server_sock < 0) return NULL;

  int one = 1;
  setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9091); // Porta de retorno local
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");

  if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server_sock, 4) < 0) {
    close(server_sock);
    return NULL;
  }

  logf_("Servidor de retorno TCP rodando na porta 9091...");

  for (;;) {
    int client_sock = accept(server_sock, NULL, NULL);
    if (client_sock < 0) continue;

    logf_("Payload conectado de volta ao servidor local!");
    
    pthread_mutex_lock(&url_mutex);
    if (strlen(current_target_url) > 0) {
      // Envia a URL do pacote diretamente para o payload via socket de retorno
      send_all(client_sock, current_target_url, (int)strlen(current_target_url));
      send_all(client_sock, "\n", 1);
      logf_("URL enviada com sucesso para o payload de retorno!");
    }
    pthread_mutex_unlock(&url_mutex);

    close(client_sock);
  }

  close(server_sock);
  return NULL;
}

// Função que prepara o payload, injeta o IP/Porta de retorno (marcador B4) e envia ao BinLoader[cite: 5]
static int trigger_bgft_install(const char *pkg_url) {
  logf_("Iniciando despacho do DirectPackageInstaller...");

  int fd = open(PAYLOAD_PATH, O_RDONLY);
  if (fd < 0) {
    logf_("AVISO: directpayload.bin nao encontrado, operando em modo estavel.");
    return 0;
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

  // Procura o marcador B4 B4 B4 B4 B4 B4 para injetar o IP (127.0.0.1) e Porta (9091)[cite: 5]
  unsigned char marker[6] = {0xb4, 0xb4, 0xb4, 0xb4, 0xb4, 0xb4};
  int found = -1;
  for (int i = 0; i < size - 6; i++) {
    if (!memcmp(payload_data + i, marker, 6)) {
      found = i;
      break;
    }
  }

  if (found != -1) {
    // 127.0.0.1 em bytes = 127, 0, 0, 1
    payload_data[found + 0] = 127;
    payload_data[found + 1] = 0;
    payload_data[found + 2] = 0;
    payload_data[found + 3] = 1;
    // Porta 9091 em Little Endian (portas de rede)
    unsigned short port = 9091;
    payload_data[found + 4] = (port >> 8) & 0xFF;
    payload_data[found + 5] = port & 0xFF;
    logf_("Marcador B4 atualizado com IP e Porta de retorno!");
  }

  // Conecta na porta 9090 do BinLoader do GoldHEN para injetar o binário[cite: 5]
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock >= 0) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9090);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
      send_all(sock, payload_data, size);
      logf_("Payload enviado com sucesso para o BinLoader (9090)!");
    }
    close(sock);
  }

  free(payload_data);
  return 0;
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

    logf_("URL alvo definida com sucesso:");
    logf_(decoded_url);

    pthread_mutex_lock(&url_mutex);
    memset(current_target_url, 0, sizeof(current_target_url));
    strcpy(current_target_url, decoded_url);
    pthread_mutex_unlock(&url_mutex);

    // Aciona o mecanismo do payload de retorno
    trigger_bgft_install(decoded_url);

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
  logf_("=== INICIO DO APP DEFINITIVO (COM RETORNO TCP) ===");
  sceSystemServiceHideSplashScreen();

  // Inicia a thread do servidor TCP de retorno em segundo plano
  pthread_t callback_thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&callback_thread, &attr, callback_server_thread, NULL);
  pthread_attr_destroy(&attr);

  int s = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a; memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = inet_addr("127.0.0.1");
  
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 16) < 0) {
    logf_("ERRO FATAL: bind/listen falhou");
    return 1;
  }
  
  logf_("Servidor HTTP principal rodando na porta 8080!");
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
