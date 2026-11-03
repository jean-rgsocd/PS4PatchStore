#include "auth.h"
#include "auth_proto.h"
#include "install.h"           /* install_log */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>              /* Http.h usa bool sem incluir */

#include <orbis/Http.h>
#include <orbis/Ssl.h>
#include <orbis/Net.h>
#include <orbis/Sysmodule.h>

#define SERVER_FILE_APP   "/app0/server.url"
#define SERVER_FILE_DATA  "/data/jkpatchs_server.txt"
#define SESSION_FILE      "/data/jkpatchs.sess"
#define USER_AGENT        "Mozilla/5.0 (PLAYSTATION 4; JKPatchs/1.0)"
#define MAX_RESP          (2 * 1024 * 1024)
#define NET_POOLSIZE      (64 * 1024)

/* ------------------------------------------------------------------ estado compartilhado */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int running = 0;                      /* ha uma tarefa de rede em andamento */
static int have_ev = 0;
static AuthEvent evq;

static char server[256];                     /* sem barra final */

static void post_event(int type, const char *msg, const char *user, char *text) {
  pthread_mutex_lock(&mu);
  if (have_ev && evq.text) free(evq.text);
  memset(&evq, 0, sizeof(evq));
  evq.type = type;
  if (msg) snprintf(evq.msg, sizeof(evq.msg), "%s", msg);
  if (user) snprintf(evq.user, sizeof(evq.user), "%s", user);
  evq.text = text;
  have_ev = 1;
  running = 0;
  pthread_mutex_unlock(&mu);
}

static void logrc(const char *what, int rc) {            /* "o que" falhou + codigo em hexa */
  char m[256];
  snprintf(m, sizeof(m), "%s rc=0x%x", what, (unsigned)rc);
  install_log(m);
}

/* ------------------------------------------------------------------ configuracao */
static int read_small(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, cap - 1);
  close(fd);
  if (n <= 0) return -1;
  buf[n] = 0;
  return (int)n;
}

int auth_server_configured(void) {
  char buf[300];
  server[0] = 0;
  if (read_small(SERVER_FILE_DATA, buf, sizeof(buf)) < 0 && read_small(SERVER_FILE_APP, buf, sizeof(buf)) < 0)
    return -1;
  char *u = auth_trim(buf);
  char *nl = strchr(u, '\n'); if (nl) *nl = 0;
  u = auth_trim(u);
  if (strncmp(u, "https://", 8) != 0) {                /* nunca aceita HTTP puro: a senha seguiria aberta */
    install_log("ERRO: a URL do servidor precisa comecar com https://");
    return -1;
  }
  size_t n = strlen(u);
  while (n > 8 && u[n - 1] == '/') u[--n] = 0;
  if (n < 12 || n >= sizeof(server) || strstr(u, "SEU-WORKER")) return -1;
  snprintf(server, sizeof(server), "%s", u);
  return 0;
}

/* ------------------------------------------------------------------ sessao salva */
static int session_save(const char *user, const char *token, long exp) {
  char buf[600];
  int n = snprintf(buf, sizeof(buf), "%s\n%s\n%ld\n", user, token, exp);
  int fd = open(SESSION_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  ssize_t w = write(fd, buf, (size_t)n);
  close(fd);
  return w == n ? 0 : -1;
}

static int session_load(char *user, size_t ucap, char *token, size_t tcap) {
  char buf[600];
  if (read_small(SESSION_FILE, buf, sizeof(buf)) < 0) return -1;
  char *l1 = buf, *l2 = strchr(l1, '\n');
  if (!l2) return -1;
  *l2++ = 0;
  char *l3 = strchr(l2, '\n');
  if (!l3) return -1;
  *l3 = 0;
  if (!l1[0] || !l2[0] || strlen(l1) >= ucap || strlen(l2) >= tcap) return -1;
  snprintf(user, ucap, "%s", l1);
  snprintf(token, tcap, "%s", l2);
  return 0;
}

void auth_logout(void) { unlink(SESSION_FILE); }

/* ------------------------------------------------------------------ HTTPS */
static int net_ready = 0, net_failed = 0;
static int net_mem = 0, ssl_ctx = 0, http_ctx = 0;

static int net_init(void) {
  if (net_ready) return 0;
  if (net_failed) return -1;
  int r;
  if ((r = (int)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET)) < 0) { logrc("net: sysmodule NET", r); goto fail; }
  if ((r = (int)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP)) < 0) { logrc("net: sysmodule HTTP", r); goto fail; }
  if ((r = (int)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL)) < 0) { logrc("net: sysmodule SSL", r); goto fail; }
  if ((r = sceNetInit()) < 0) { logrc("net: sceNetInit", r); goto fail; }
  net_mem = sceNetPoolCreate("jkpatchs", NET_POOLSIZE, 0);
  if (net_mem < 0) { logrc("net: sceNetPoolCreate", net_mem); goto fail; }
  ssl_ctx = sceSslInit(SSL_POOLSIZE);
  if (ssl_ctx < 0) { logrc("net: sceSslInit", ssl_ctx); goto fail; }
  http_ctx = sceHttpInit(net_mem, ssl_ctx, LIBHTTP_POOLSIZE);
  if (http_ctx < 0) { logrc("net: sceHttpInit", http_ctx); goto fail; }
  net_ready = 1;
  install_log("Rede iniciada (HTTPS com verificacao de certificado).");
  return 0;
fail:
  net_failed = 1;
  return -1;
}

static void describe_error(int rc, char *err, size_t cap) {
  unsigned u = (unsigned)rc;
  if ((u & 0xFFFF0000u) == 0x80950000u)                    /* erros do libSsl: certificado recusado */
    snprintf(err, cap, "Não foi possível validar o certificado do servidor (0x%08X).", u);
  else if ((u & 0xFFFF0000u) == 0x80430000u)               /* erros do libHttp: DNS, conexao, tempo esgotado */
    snprintf(err, cap, "Sem conexão com o servidor (0x%08X). Verifique a internet do PS4.", u);
  else
    snprintf(err, cap, "Falha de rede (0x%08X).", u);
}

/* Faz uma requisicao. Em caso de sucesso retorna 0 e *resp (malloc, terminada em 0) / *status. */
static int http_do(int post, const char *path, const char *body, const char *bearer,
                   int *status, char **resp, char *err, size_t ecap) {
  *resp = NULL; *status = 0;
  if (net_init() != 0) { snprintf(err, ecap, "Não foi possível iniciar a rede do app."); return -1; }

  char url[600];
  snprintf(url, sizeof(url), "%s%s", server, path);
  int tpl = -1, conn = -1, req = -1, rc = 0;
  char *buf = NULL;
  size_t len = 0, cap = 0;

  tpl = sceHttpCreateTemplate(http_ctx, USER_AGENT, ORBIS_HTTP_VERSION_1_1, 1);
  if (tpl < 0) { rc = tpl; logrc("http: CreateTemplate", rc); goto out; }
  sceHttpSetResolveTimeOut(tpl, 10 * 1000 * 1000);
  sceHttpSetConnectTimeOut(tpl, 15 * 1000 * 1000);
  sceHttpSetSendTimeOut(tpl, 15 * 1000 * 1000);
  /* SEM sceHttpsSetSslCallback: o certificado do servidor e verificado de verdade. */

  conn = sceHttpCreateConnectionWithURL(tpl, url, 0);
  if (conn < 0) { rc = conn; logrc("http: CreateConnection", rc); goto out; }
  size_t blen = body ? strlen(body) : 0;
  req = sceHttpCreateRequestWithURL(conn, post ? ORBIS_METHOD_POST : ORBIS_METHOD_GET, url, blen);
  if (req < 0) { rc = req; logrc("http: CreateRequest", rc); goto out; }

  if (post) sceHttpAddRequestHeader(req, "Content-Type", "application/x-www-form-urlencoded", 0);
  if (bearer && bearer[0]) {
    char h[400];
    snprintf(h, sizeof(h), "Bearer %s", bearer);
    sceHttpAddRequestHeader(req, "Authorization", h, 0);
  }
  rc = sceHttpSendRequest(req, body, blen);
  if (rc < 0) { { char w[96]; snprintf(w, sizeof(w), "http: SendRequest %s", path); logrc(w, rc); }; goto out; }
  rc = sceHttpGetStatusCode(req, status);
  if (rc < 0) { logrc("http: GetStatusCode", rc); goto out; }

  for (;;) {
    if (len + 4097 > cap) {
      size_t nc = cap ? cap * 2 : 8192;
      if (nc > MAX_RESP + 8192) break;
      char *nb = (char *)realloc(buf, nc);
      if (!nb) { rc = -1; break; }
      buf = nb; cap = nc;
    }
    int n = sceHttpReadData(req, buf + len, 4096);
    if (n < 0) { rc = n; logrc("http: ReadData", rc); break; }
    if (n == 0) break;
    len += (size_t)n;
    if (len > MAX_RESP) { rc = -1; break; }
  }
  if (rc >= 0) rc = 0;

out:
  if (req >= 0) sceHttpDeleteRequest(req);
  if (conn >= 0) sceHttpDeleteConnection(conn);
  if (tpl >= 0) sceHttpDeleteTemplate(tpl);
  if (rc != 0) {
    free(buf);
    describe_error(rc, err, ecap);
    return -1;
  }
  if (!buf) buf = (char *)calloc(1, 1);
  else buf[len] = 0;
  *resp = buf;
  { char w[96]; snprintf(w, sizeof(w), "http: %s -> HTTP %d", path, *status); install_log(w); }
  return 0;
}

/* Baixa uma URL https qualquer (video) direto para um arquivo, com porcentagem. Verificacao de certificado ligada. */
int auth_download(const char *url, const char *dest, volatile int *pct, volatile int *cancel, char *err, int cap) {
  if (strncmp(url, "https://", 8) != 0) { snprintf(err, (size_t)cap, "Link de vídeo inválido (precisa ser https)."); return -1; }
  if (net_init() != 0) { snprintf(err, (size_t)cap, "Não foi possível iniciar a rede do app."); return -1; }
  int tpl = -1, conn = -1, req = -1, rc = 0, status = 0;
  FILE *f = NULL;
  unsigned char *buf = (unsigned char *)malloc(16384);
  if (!buf) { snprintf(err, (size_t)cap, "Sem memória."); return -1; }
  *pct = 0;
  tpl = sceHttpCreateTemplate(http_ctx, USER_AGENT, ORBIS_HTTP_VERSION_1_1, 1);
  if (tpl < 0) { rc = tpl; goto out; }
  sceHttpSetResolveTimeOut(tpl, 10 * 1000 * 1000);
  sceHttpSetConnectTimeOut(tpl, 15 * 1000 * 1000);
  sceHttpSetSendTimeOut(tpl, 15 * 1000 * 1000);
  conn = sceHttpCreateConnectionWithURL(tpl, url, 0);
  if (conn < 0) { rc = conn; goto out; }
  req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, url, 0);
  if (req < 0) { rc = req; goto out; }
  rc = sceHttpSendRequest(req, NULL, 0);
  if (rc < 0) { logrc("video: SendRequest", rc); goto out; }
  rc = sceHttpGetStatusCode(req, &status);
  if (rc < 0) goto out;
  if (status != 200) {
    snprintf(err, (size_t)cap, status == 404 ? "Vídeo não encontrado no servidor (404)." : "O servidor recusou o vídeo (HTTP %d).", status);
    { char w[96]; snprintf(w, sizeof(w), "video: HTTP %d", status); install_log(w); }
    rc = 0; status = -1; goto out;
  }
  int cl_res = 0;
  size_t total = 0;
  if (sceHttpGetResponseContentLength(req, &cl_res, &total) < 0 || cl_res != 0) total = 0;
  f = fopen(dest, "wb");
  if (!f) { snprintf(err, (size_t)cap, "Não foi possível gravar o vídeo no console."); rc = 0; status = -1; goto out; }
  size_t got = 0;
  for (;;) {
    if (*cancel) { rc = 0; status = -2; break; }
    int n = sceHttpReadData(req, buf, 16384);
    if (n < 0) { rc = n; logrc("video: ReadData", rc); break; }
    if (n == 0) break;
    if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { snprintf(err, (size_t)cap, "Sem espaço para gravar o vídeo."); rc = 0; status = -1; break; }
    got += (size_t)n;
    if (total > 0) *pct = (int)((got * 100) / total);
  }
  if (rc == 0 && status == 200) *pct = 100;
out:
  if (f) fclose(f);
  if (req >= 0) sceHttpDeleteRequest(req);
  if (conn >= 0) sceHttpDeleteConnection(conn);
  if (tpl >= 0) sceHttpDeleteTemplate(tpl);
  free(buf);
  if (rc != 0) { describe_error(rc, err, (size_t)cap); return -1; }
  if (status == -2) return -1;                        /* cancelado pelo usuario (sem mensagem) */
  if (status != 200) return -1;                       /* err ja preenchido */
  return 0;
}

/* ------------------------------------------------------------------ tarefas */
static int fetch_catalog(const char *user, const char *token, int silent_login_needed) {
  char err[200] = "";
  char *resp = NULL;
  int st = 0;
  if (http_do(0, "/catalog", NULL, token, &st, &resp, err, sizeof(err)) != 0) {
    post_event(AUTH_EV_CATALOG_FAIL, err, user, NULL);
    return -1;
  }
  if (st == 401 || st == 403) {
    free(resp);
    auth_logout();
    post_event(AUTH_EV_NEED_LOGIN, silent_login_needed ? "Sua sessão expirou. Entre novamente." : "Sessão recusada.", NULL, NULL);
    return -1;
  }
  if (st != 200 || !resp[0] || !strncmp(resp, "ERRO", 4)) {
    char m[200];
    if (!strncmp(resp, "ERRO\n", 5)) { snprintf(m, sizeof(m), "%s", resp + 5); char *nl = strchr(m, '\n'); if (nl) *nl = 0; }
    else snprintf(m, sizeof(m), "Catálogo indisponível (HTTP %d).", st);
    free(resp);
    post_event(AUTH_EV_CATALOG_FAIL, m, user, NULL);
    return -1;
  }
  post_event(AUTH_EV_CATALOG, NULL, user, resp);
  return 0;
}

static void *restore_thread(void *arg) {
  (void)arg;
  char user[64], token[300];
  if (session_load(user, sizeof(user), token, sizeof(token)) != 0) {
    post_event(AUTH_EV_NEED_LOGIN, NULL, NULL, NULL);
    return NULL;
  }
  install_log("Sessao salva encontrada; baixando catalogo.");
  fetch_catalog(user, token, 1);
  return NULL;
}

typedef struct { char user[64], pass[128]; } LoginJob;

static void *login_thread(void *arg) {
  LoginJob *j = (LoginJob *)arg;
  char body[600], err[200] = "", token[300] = "";
  char *resp = NULL;
  int st = 0;
  long exp = 0;

  if (auth_build_login_body(j->user, j->pass, body, sizeof(body)) != 0) {
    post_event(AUTH_EV_LOGIN_FAIL, "Usuário ou senha longos demais.", NULL, NULL);
  } else if (http_do(1, "/login", body, NULL, &st, &resp, err, sizeof(err)) != 0) {
    post_event(AUTH_EV_LOGIN_FAIL, err, NULL, NULL);
  } else {
    if (auth_parse_login(st, resp, token, sizeof(token), &exp, err, sizeof(err)) != 0) {
      post_event(AUTH_EV_LOGIN_FAIL, err, NULL, NULL);
    } else {
      char user[64];
      snprintf(user, sizeof(user), "%s", j->user);
      for (char *p = user; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
      session_save(user, token, exp);
      install_log("Login aceito.");
      fetch_catalog(user, token, 0);
    }
    free(resp);
  }
  memset(j, 0, sizeof(*j));                      /* nao deixa a senha na memoria */
  free(j);
  return NULL;
}

static int spawn(void *(*fn)(void *), void *arg) {
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int r = pthread_create(&t, &at, fn, arg);
  pthread_attr_destroy(&at);
  return r;
}

static int begin(void) {
  pthread_mutex_lock(&mu);
  int ok = !running;
  if (ok) running = 1;
  pthread_mutex_unlock(&mu);
  return ok;
}

void auth_start_restore(void) {
  if (auth_server_configured() != 0) {
    post_event(AUTH_EV_NEED_LOGIN, "Servidor não configurado (arquivo server.url).", NULL, NULL);
    return;
  }
  if (!begin()) return;
  if (spawn(restore_thread, NULL) != 0) post_event(AUTH_EV_CATALOG_FAIL, "Não foi possível iniciar a rede.", NULL, NULL);
}

void auth_start_login(const char *user, const char *pass) {
  if (auth_server_configured() != 0) {
    post_event(AUTH_EV_LOGIN_FAIL, "Servidor não configurado (arquivo server.url).", NULL, NULL);
    return;
  }
  if (!begin()) return;
  LoginJob *j = (LoginJob *)calloc(1, sizeof(*j));
  if (!j) { post_event(AUTH_EV_LOGIN_FAIL, "Sem memória.", NULL, NULL); return; }
  snprintf(j->user, sizeof(j->user), "%s", user);
  snprintf(j->pass, sizeof(j->pass), "%s", pass);
  if (spawn(login_thread, j) != 0) {
    free(j);
    post_event(AUTH_EV_LOGIN_FAIL, "Não foi possível iniciar a rede.", NULL, NULL);
  }
}

int auth_poll(AuthEvent *ev) {
  pthread_mutex_lock(&mu);
  int has = have_ev;
  if (has) { *ev = evq; have_ev = 0; memset(&evq, 0, sizeof(evq)); }
  pthread_mutex_unlock(&mu);
  return has;
}
