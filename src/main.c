/* eboot.bin (esqueleto, NAO testado em hardware) - OpenOrbis Toolchain.
 * Serve /app0/www em 127.0.0.1:8080 e abre o navegador do sistema nessa URL. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern int sceSystemServiceLaunchWebBrowser(const char *uri);   /* libSceSystemService */

#define PORT 8080
#ifndef START_URL
#define START_URL "http://127.0.0.1:8080/"   /* troque por https://seu-site/ se o local falhar */
#endif
#define LOG "/data/ps4store.log"

static void logf_(const char *m) {
  int f = open(LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (f >= 0) { write(f, m, strlen(m)); write(f, "\n", 1); close(f); }
}
#define ROOT "/app0/www"

static const char *mime(const char *p) {
  const char *e = strrchr(p, '.');
  if (!e) return "application/octet-stream";
  if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
  if (!strcmp(e, ".css"))  return "text/css";
  if (!strcmp(e, ".js"))   return "application/javascript";
  if (!strcmp(e, ".png"))  return "image/png";
  if (!strcmp(e, ".jpg"))  return "image/jpeg";
  return "application/octet-stream";
}

static void serve(int c) {
  char req[1024], path[256], full[300], buf[4096], hdr[256];
  int n = read(c, req, sizeof(req) - 1);
  if (n <= 0) return;
  req[n] = 0;
  if (sscanf(req, "GET %255s", path) != 1 || strstr(path, "..")) return;
  char *q = strchr(path, '?'); if (q) *q = 0;
  if (!strcmp(path, "/")) strcpy(path, "/index.html");
  snprintf(full, sizeof(full), "%s%s", ROOT, path);
  int fd = open(full, O_RDONLY);
  if (fd < 0) { const char *r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"; write(c, r, strlen(r)); return; }
  snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", mime(full));
  write(c, hdr, strlen(hdr));
  while ((n = read(fd, buf, sizeof(buf))) > 0) write(c, buf, n);
  close(fd);
}

int main(void) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a; memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 8) < 0) { logf_("ERRO: bind/listen falhou"); return 1; }
  logf_("servidor ok, abrindo navegador");
  if (sceSystemServiceLaunchWebBrowser(START_URL) != 0) logf_("ERRO: LaunchWebBrowser falhou");
  for (;;) { int c = accept(s, NULL, NULL); if (c >= 0) { serve(c); close(c); } }
  return 0;
}
