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
#include <time.h>
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
#define CATALOG_CACHE     "/data/jkpatchs_catalog.cache"
#define NET_POOLSIZE      (64 * 1024)

/* ------------------------------------------------------------------ estado compartilhado */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int running = 0;                      /* ha uma tarefa de rede em andamento */
static int have_ev = 0;
static AuthEvent evq;

static int g_offline = 0;
static long g_support = 0;                   /* prazo do apoio (lido do /me), entregue junto do evento */
static int g_nag = 0;
static char g_state[16], g_plan_name[16], g_plans[1500];
static long g_expires = 0;
static int g_registered = 0;
static char server[256];                     /* sem barra final */

static void post_event2(int type, const char *msg, const char *user, char *text, const char *plan, long plan_now) {
  pthread_mutex_lock(&mu);
  if (have_ev && evq.text) free(evq.text);
  memset(&evq, 0, sizeof(evq));
  evq.type = type;
  if (msg) snprintf(evq.msg, sizeof(evq.msg), "%s", msg);
  if (user) snprintf(evq.user, sizeof(evq.user), "%s", user);
  if (plan) snprintf(evq.plan, sizeof(evq.plan), "%s", plan);
  evq.plan_now = plan_now;
  evq.support_until = g_support;
  evq.nag = g_nag;
  snprintf(evq.state, sizeof(evq.state), "%s", g_state);
  snprintf(evq.plan_name, sizeof(evq.plan_name), "%s", g_plan_name);
  snprintf(evq.plans, sizeof(evq.plans), "%s", g_plans);
  evq.expires = g_expires;
  evq.registered = g_registered;
  g_registered = 0;
  evq.offline = g_offline;
  evq.text = text;
  have_ev = 1;
  running = 0;
  pthread_mutex_unlock(&mu);
}

static void post_event(int type, const char *msg, const char *user, char *text) {
  post_event2(type, msg, user, text, NULL, 0);
}

static void logrc(const char *what, int rc) {            /* "o que" falhou + codigo em hexa */
  char m[256];
  snprintf(m, sizeof(m), "%s rc=0x%x", what, (unsigned)rc);
  install_log(m);
}

/* ------------------------------------------------------------------ ID do console */
/* O servidor prende a conta (e o teste gratis de 1 hora) a este ID. Usa o OpenPSID do PS4; se o sistema nao entregar,
 * cai para um ID aleatorio guardado em /data (so perde se o usuario apagar os dados do app). */
extern int jk_get_openpsid(unsigned char *id) __asm__("sceKernelGetOpenPsId") __attribute__((weak));
#define DEV_FILE "/data/jkpatchs_dev.txt"

static const char *dev_id(void) {
  static char dev[40];
  static int done = 0;
  if (done) return dev;
  done = 1;
  unsigned char raw[16];
  memset(raw, 0, sizeof(raw));
  if (jk_get_openpsid) {
    int r = jk_get_openpsid(raw);
    int nz = 0, nf = 0;
    for (int i = 0; i < 16; i++) { if (raw[i]) nz = 1; if (raw[i] != 0xFF) nf = 1; }
    if (r >= 0 && nz && nf) {
      for (int i = 0; i < 16; i++) snprintf(dev + i * 2, 3, "%02x", raw[i]);
      install_log("ID do console: OpenPSID lido.");
      return dev;
    }
    logrc("ID do console: OpenPSID indisponivel", r);
  } else {
    install_log("ID do console: funcao OpenPSID ausente neste sistema.");
  }
  char buf[64];
  int fd = open(DEV_FILE, O_RDONLY);
  if (fd >= 0) {
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n >= 32) { memcpy(dev, buf, 32); dev[32] = 0; install_log("ID do console: usando o ID salvo."); return dev; }
  }
  uint64_t x = (uint64_t)(uintptr_t)&x ^ ((uint64_t)time(NULL) << 20) ^ ((uint64_t)clock() << 7) ^ 0x9E3779B97F4A7C15ULL;
  for (int i = 0; i < 16; i++) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    snprintf(dev + i * 2, 3, "%02x", (unsigned)((x >> 24) & 0xFF));
  }
  fd = open(DEV_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd >= 0) { write(fd, dev, 32); close(fd); }
  install_log("ID do console: gerado um ID novo (OpenPSID indisponivel).");
  return dev;
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
  if (strncmp(url, "https://", 8) != 0) { snprintf(err, (size_t)cap, "Link inválido (precisa ser https)."); return -1; }
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
    snprintf(err, (size_t)cap, status == 404 ? "Arquivo não encontrado no servidor (404)." : "O servidor recusou o arquivo (HTTP %d).", status);
    { char w[96]; snprintf(w, sizeof(w), "download: HTTP %d", status); install_log(w); }
    rc = 0; status = -1; goto out;
  }
  int cl_res = 0;
  size_t total = 0;
  if (sceHttpGetResponseContentLength(req, &cl_res, &total) < 0 || cl_res != 0) total = 0;
  { char w[96]; snprintf(w, sizeof(w), "download: HTTP 200, tamanho %llu bytes", (unsigned long long)total); install_log(w); }
  f = fopen(dest, "wb");
  if (!f) { snprintf(err, (size_t)cap, "Não foi possível gravar o arquivo no console."); install_log("download: fopen falhou (sem permissao/espaco?)"); rc = 0; status = -1; goto out; }
  size_t got = 0;
  for (;;) {
    if (*cancel) { rc = 0; status = -2; break; }
    int n = sceHttpReadData(req, buf, 16384);
    if (n < 0) { rc = n; logrc("video: ReadData", rc); break; }
    if (n == 0) break;
    if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { snprintf(err, (size_t)cap, "Sem espaço para gravar o arquivo."); install_log("download: fwrite falhou (disco cheio?)"); rc = 0; status = -1; break; }
    got += (size_t)n;
    if (total > 0) *pct = (int)((got * 100) / total);
  }
  if (rc == 0 && status == 200 && total > 0 && got != total) {          /* conexao caiu no meio: o servidor prometeu mais bytes */
    snprintf(err, (size_t)cap, "Download incompleto. Tente de novo.");
    { char w[120]; snprintf(w, sizeof(w), "download: INCOMPLETO (recebeu %llu de %llu bytes)", (unsigned long long)got, (unsigned long long)total); install_log(w); }
    status = -1;
  }
  if (rc == 0 && status == 200) *pct = 100;
  { char w[96]; snprintf(w, sizeof(w), "download: gravou %llu bytes (rc=%d)", (unsigned long long)got, rc); install_log(w); }
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
/* Copia do ultimo catalogo recebido (1a linha "# user<TAB>nome"): se o servidor cair, a loja continua aparecendo. */
static void cache_save(const char *user, const char *text) {
  char tmp[sizeof(CATALOG_CACHE) + 8];
  snprintf(tmp, sizeof(tmp), "%s.tmp", CATALOG_CACHE);
  FILE *f = fopen(tmp, "wb");
  if (!f) return;
  size_t n = strlen(text);
  int ok = fprintf(f, "# user\t%s\n", user) > 0 && fwrite(text, 1, n, f) == n;
  ok = (fclose(f) == 0) && ok;
  if (ok) rename(tmp, CATALOG_CACHE); else unlink(tmp);
}

/* Servidor indisponivel: usa a copia salva (so se for do mesmo usuario / visitante). Retorna 1 se mostrou. */
static int cache_use(const char *user) {
  FILE *f = fopen(CATALOG_CACHE, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 16 || n > MAX_RESP) { fclose(f); return 0; }
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) { fclose(f); return 0; }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = 0;
  char want[100];
  snprintf(want, sizeof(want), "# user\t%s\n", user);
  if (strncmp(buf, want, strlen(want)) != 0) { free(buf); return 0; }
  g_support = 0; g_nag = 0; g_offline = 1; g_expires = 0;
  g_state[0] = 0; g_plan_name[0] = 0; g_plans[0] = 0;
  install_log("Servidor indisponivel: usando a ultima loja salva no console.");
  post_event2(AUTH_EV_CATALOG, NULL, user, buf, "", 0);
  g_offline = 0;
  return 1;
}

static int fetch_catalog(const char *user, const char *token, int silent_login_needed, int preview) {
  char err[200] = "";
  char *resp = NULL;
  int st = 0;
  if (http_do(0, preview ? "/preview" : "/catalog", NULL, token, &st, &resp, err, sizeof(err)) != 0) {
    if (!preview && cache_use(user)) return 0;
    post_event(AUTH_EV_CATALOG_FAIL, err, user, NULL);
    return -1;
  }
  if (st == 401 || st == 403) {
    free(resp);
    auth_logout();
    post_event(AUTH_EV_NEED_LOGIN, silent_login_needed ? "Sua sessão expirou. Entre novamente." : "Sessão recusada.", NULL, NULL);
    return -1;
  }
  if ((st >= 500 || st == 429) && !preview && cache_use(user)) { free(resp); return 0; }
  if (st != 200 || !resp[0] || !strncmp(resp, "ERRO", 4)) {
    char m[200];
    if (!strncmp(resp, "ERRO\n", 5)) { snprintf(m, sizeof(m), "%s", resp + 5); char *nl = strchr(m, '\n'); if (nl) *nl = 0; }
    else snprintf(m, sizeof(m), "Catálogo indisponível (HTTP %d).", st);
    free(resp);
    post_event(AUTH_EV_CATALOG_FAIL, m, user, NULL);
    return -1;
  }
  if (!preview) cache_save(user, resp);
  /* Plano do usuario (para o topo do app). Se falhar, o app so nao mostra o plano. */
  char plan[48] = "";
  long now = 0;
  g_support = 0; g_nag = 0; g_expires = 0;
  g_state[0] = 0; g_plan_name[0] = 0; g_plans[0] = 0;
  if (!preview) {
    char e2[200] = "";
    char *r2 = NULL;
    int s2 = 0;
    if (http_do(0, "/me", NULL, token, &s2, &r2, e2, sizeof(e2)) == 0) {
      AuthMe me;
      if (auth_parse_me2(s2, r2, &me) == 0) {
        snprintf(plan, sizeof(plan), "%s", me.plan);
        now = me.now;
        g_support = me.support; g_nag = me.nag; g_expires = me.expires;
        snprintf(g_state, sizeof(g_state), "%s", me.state);
        snprintf(g_plan_name, sizeof(g_plan_name), "%s", me.plan_name);
      }
      free(r2);
    }
    {                                                  /* precos e Pix atuais (opcional: sem isso o app usa os que vieram nele) */
      char e3[200] = "";
      char *r3 = NULL;
      int s3 = 0;
      if (http_do(0, "/plans", NULL, NULL, &s3, &r3, e3, sizeof(e3)) == 0) {
        if (s3 == 200 && !strncmp(r3, "OK\n", 3) && strlen(r3) < sizeof(g_plans)) snprintf(g_plans, sizeof(g_plans), "%s", r3);
        free(r3);
      }
    }
  }
  post_event2(AUTH_EV_CATALOG, NULL, user, resp, plan, now);
  g_registered = 0;
  return 0;
}

static void *preview_thread(void *arg) {
  (void)arg;
  install_log("Modo visitante: baixando catalogo so para olhar.");
  fetch_catalog("", NULL, 0, 1);
  return NULL;
}

static void *restore_thread(void *arg) {
  (void)arg;
  char user[64], token[300];
  if (session_load(user, sizeof(user), token, sizeof(token)) != 0) {
    post_event(AUTH_EV_NEED_LOGIN, NULL, NULL, NULL);
    return NULL;
  }
  install_log("Sessao salva encontrada; baixando catalogo.");
  fetch_catalog(user, token, 1, 0);
  return NULL;
}

typedef struct { char user[64], pass[128]; } LoginJob;

static void *login_thread(void *arg) {
  LoginJob *j = (LoginJob *)arg;
  char body[600], err[200] = "", token[300] = "";
  char *resp = NULL;
  int st = 0;
  long exp = 0;

  if (auth_build_login_body_dev(j->user, j->pass, dev_id(), body, sizeof(body)) != 0) {
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
      fetch_catalog(user, token, 0, 0);
    }
    free(resp);
  }
  memset(j, 0, sizeof(*j));                      /* nao deixa a senha na memoria */
  free(j);
  return NULL;
}

static void *register_thread(void *arg) {
  LoginJob *j = (LoginJob *)arg;
  char body[600], err[200] = "", token[300] = "";
  char *resp = NULL;
  int st = 0;
  long exp = 0;

  if (auth_build_login_body_dev(j->user, j->pass, dev_id(), body, sizeof(body)) != 0) {
    post_event(AUTH_EV_REGISTER_FAIL, "Usuário ou senha longos demais.", NULL, NULL);
  } else if (http_do(1, "/register", body, NULL, &st, &resp, err, sizeof(err)) != 0) {
    post_event(AUTH_EV_REGISTER_FAIL, err, NULL, NULL);
  } else {
    if (auth_parse_login(st, resp, token, sizeof(token), &exp, err, sizeof(err)) != 0) {   /* "OK\ncadastrado\n<estado>" */
      post_event(AUTH_EV_REGISTER_FAIL, err, NULL, NULL);
      free(resp);
    } else {
      free(resp);
      install_log("Cadastro criado; entrando na conta.");
      /* servidor novo: ja libera o teste gratis. Entra direto (mesmos dados) e a janela de planos abre por cima. */
      char *r2 = NULL;
      char e2[200] = "";
      int s2 = 0;
      if (http_do(1, "/login", body, NULL, &s2, &r2, e2, sizeof(e2)) == 0 &&
          auth_parse_login(s2, r2, token, sizeof(token), &exp, e2, sizeof(e2)) == 0) {
        char user[64];
        snprintf(user, sizeof(user), "%s", j->user);
        for (char *p = user; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        session_save(user, token, exp);
        g_registered = 1;
        free(r2);
        fetch_catalog(user, token, 0, 0);
      } else {
        free(r2);
        post_event(AUTH_EV_LOGIN_FAIL, "Cadastro criado! Toque em ENTRAR.", NULL, NULL);
      }
    }
  }
  memset(j, 0, sizeof(*j));
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

void auth_start_preview(void) {
  if (auth_server_configured() != 0) {
    post_event(AUTH_EV_CATALOG_FAIL, "Servidor não configurado (arquivo server.url).", NULL, NULL);
    return;
  }
  if (!begin()) return;
  if (spawn(preview_thread, NULL) != 0) post_event(AUTH_EV_CATALOG_FAIL, "Não foi possível iniciar a rede.", NULL, NULL);
}

void auth_start_register(const char *user, const char *pass) {
  if (auth_server_configured() != 0) {
    post_event(AUTH_EV_REGISTER_FAIL, "Servidor não configurado (arquivo server.url).", NULL, NULL);
    return;
  }
  if (!begin()) return;
  LoginJob *j = (LoginJob *)calloc(1, sizeof(*j));
  if (!j) { post_event(AUTH_EV_REGISTER_FAIL, "Sem memória.", NULL, NULL); return; }
  snprintf(j->user, sizeof(j->user), "%s", user);
  snprintf(j->pass, sizeof(j->pass), "%s", pass);
  if (spawn(register_thread, j) != 0) {
    free(j);
    post_event(AUTH_EV_REGISTER_FAIL, "Não foi possível iniciar a rede.", NULL, NULL);
  }
}

int auth_poll(AuthEvent *ev) {
  pthread_mutex_lock(&mu);
  int has = have_ev;
  if (has) { *ev = evq; have_ev = 0; memset(&evq, 0, sizeof(evq)); }
  pthread_mutex_unlock(&mu);
  return has;
}
