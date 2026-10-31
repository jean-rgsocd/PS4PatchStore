#include "install.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define CALLBACK_PORT 9091
#define BINLOADER     9090
#define LOG           "/data/ps4store.log"
#define PAYLOAD_PATH  "/app0/directpayload.bin"

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void install_log(const char *m) {
  pthread_mutex_lock(&log_mutex);                  /* evita linhas misturadas entre threads */
  int f = open(LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (f >= 0) { write(f, m, strlen(m)); write(f, "\n", 1); close(f); }
  pthread_mutex_unlock(&log_mutex);
}

static int send_all_ok(int c, const char *p, int n) {
  while (n > 0) {
    int w = write(c, p, n);
    if (w <= 0) return -1;
    p += w; n -= w;
  }
  return 0;
}

/* ---------- conexao de retorno do payload ---------- */
static int waiting_sock = -1;
static pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *callback_server_thread(void *arg) {
  (void)arg;
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
    install_log("ERRO: porta 9091 indisponivel");
    close(s);
    return NULL;
  }
  install_log("Servidor de retorno ativo na porta 9091.");

  for (;;) {
    int c = accept(s, NULL, NULL);
    if (c < 0) continue;
    pthread_mutex_lock(&sock_mutex);
    if (waiting_sock >= 0) close(waiting_sock);
    waiting_sock = c;               /* payload esta bloqueado lendo; nao enviamos nada agora */
    pthread_mutex_unlock(&sock_mutex);
    install_log("Payload conectou e aguarda pacote.");
  }
  return NULL;
}

void install_start_server(void) {
  pthread_t cb;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&cb, &attr, callback_server_thread, NULL);
  pthread_attr_destroy(&attr);
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
  if (fd < 0) { install_log("ERRO: /app0/directpayload.bin nao encontrado"); return -1; }

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
  if (found < 0) { install_log("ERRO: marcador B4 nao encontrado no payload"); free(pl); return -1; }

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
      install_log("ERRO: BinLoader (9090) recusou conexao. Ative o BinLoader no GoldHEN.");
    }
    close(sk);
  }
  free(pl);
  if (ok == 0) install_log("Payload enviado ao BinLoader (9090).");
  return ok;
}

/* Tipo do pacote: aceita apenas "PS4" + 2 letras/digitos maiusculos; senao PS4GD (padrao antigo). */
void install_sanitize_type(const char *in, char *out) {
  int ok = (strlen(in) == 5 && !strncmp(in, "PS4", 3));
  for (int i = 3; ok && i < 5; i++) {
    char ch = in[i];
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) ok = 0;
  }
  if (ok) { memcpy(out, in, 6); return; }   /* inclui o \0 */
  if (in[0]) install_log("AVISO: tipo invalido, usando PS4GD");
  strcpy(out, "PS4GD");
}

int install_pkg(const char *url, const char *name, const char *id, const char *type_in,
                unsigned long long size, char *err, int errsz) {
  char type[8];
  install_sanitize_type(type_in ? type_in : "", type);
  install_log(url);
  install_log(type);

  unsigned char buf[4096];
  int o = 0;
  put_u32(buf, &o, 1u);
  put_str(buf, &o, url, 2000);
  put_str(buf, &o, (name && name[0]) ? name : "PS4 Package", 200);
  put_str(buf, &o, id, 47);
  put_str(buf, &o, type, 15);
  put_u64(buf, &o, size);
  put_u32(buf, &o, 0u);            /* sem icone */

  if (deliver_job(buf, o) == 0) { install_log("Pacote entregue ao payload ja ativo."); return 0; }

  if (launch_payload() != 0) {
    snprintf(err, errsz, "Payload nao enviado. O BinLoader do GoldHEN esta ligado?");
    return -1;
  }
  for (int i = 0; i < 30; i++) {      /* ate 15 s */
    usleep(500000);
    if (deliver_job(buf, o) == 0) { install_log("Pacote entregue ao payload."); return 0; }
  }
  snprintf(err, errsz, "O payload nao conectou em 15 s.");
  install_log("ERRO: payload nao conectou em 15s");
  return -2;
}
