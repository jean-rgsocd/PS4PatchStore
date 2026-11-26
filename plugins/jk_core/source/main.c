/*
 * JK CORE - plugin GoldHEN que carrega os modulos JK PATCHS LICENCIADOS (fase J: PRX sem codigo-fonte).
 *
 * No plugins.ini fica SO o jk_core.prx. As PRX antigas ficam no console CIFRADAS (/data/jk/core/<modulo>.jkm,
 * feitas com plataforma/lic_c/cifra_modulo). A cada abertura do jogo o Core:
 *   1. le /data/jk/lic/indice.txt (modulos liberados, gravado pelo app JK PATCHS no /lic/sync)
 *   2. para cada modulo: confere o .jkm (jogo certo), a licenca (/data/jk/lic/<modulo>.lic: assinatura
 *      Ed25519 do servidor, este console, validade, data voltada) e a chave (/data/jk/lic/<modulo>.key)
 *   3. decifra para um arquivo temporario, carrega (sceKernelLoadStartModule), APAGA o temporario e chama
 *      o plugin_load() do modulo - igual ao que o GoldHEN faria.
 * Sem licenca/chave valida: o modulo nao carrega e o motivo vai para /data/jk/core/core.log (sem nada sensivel).
 *
 * Regra do projeto (v3.01 do VAR): plugin_load() SO cria a thread; todo o trabalho e na thread.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>
#include "GoldHEN.h"
#include "jk_lic.h"
#include "jk_modulo.h"
#include "lic_pub.h"

#define attr_public __attribute__((visibility("default")))
#define attr_module_hidden __attribute__((weak)) __attribute__((visibility("hidden")))

attr_public const char *g_pluginName = "jk_core";
attr_public const char *g_pluginDesc = "JK PATCHS - Core (modulos licenciados)";
attr_public const char *g_pluginAuth = "JK PATCHS";
attr_public uint32_t g_pluginVersion = 0x00000100;

#define PASTA_CORE "/data/jk/core"
#define PASTA_LIC  "/data/jk/lic"
#define ARQ_DEV    "/data/jkpatchs_dev.txt"
#define ARQ_VISTO  PASTA_LIC "/.core_t"
#define MAX_MODS   24

static OrbisPthread g_thr;
static int g_log = -1;

static void logf_(const char *fmt, ...)
{
    char b[400];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    if (g_log < 0) g_log = sceKernelOpen(PASTA_CORE "/core.log", 0x0001 | 0x0200 | 0x0400, 0666);   /* WRONLY|CREAT|TRUNC: so a sessao */
    if (g_log >= 0) sceKernelWrite(g_log, b, (size_t)n);
}

/* le um arquivo inteiro (ate max bytes). Devolve o buffer (malloc) ou NULL.
 * Nao confia no tamanho que o fstat informa: neste console o metadado do arquivo
 * pode vir desatualizado quando lido por um processo diferente de quem escreveu
 * (confirmado: fstat relatou 8 bytes para um arquivo que na verdade tem 32).
 * Por isso le direto ate o fim (EOF) ou ate "max", em vez de usar st_size. */
static unsigned char *ler_tudo(const char *p, size_t max, size_t *n_out)
{
    int fd = sceKernelOpen(p, 0, 0);
    if (fd < 0) { logf_("diag ler_tudo: open falhou p=%s fd=%d", p, fd); return NULL; }
    /* Nao aloca "max" de uma vez (alguns arquivos permitem ate 70MB de limite,
     * e um malloc desse tamanho pode falhar no plugin mesmo quando o arquivo
     * de verdade e bem menor). Comeca pequeno e vai dobrando so conforme
     * realmente le dados, ate "max". */
    size_t cap = 65536;
    if (cap > max) cap = max;
    if (cap == 0) cap = max; /* max=0 nao deveria ocorrer, mas evita cap=0 */
    unsigned char *b = (unsigned char *)malloc(cap + 1);
    if (!b) { logf_("diag ler_tudo: malloc falhou p=%s n=%zu", p, cap); sceKernelClose(fd); return NULL; }
    size_t lido = 0;
    for (;;) {
        if (lido >= cap) {
            if (cap >= max) break;
            size_t novo_cap = cap * 2;
            if (novo_cap > max) novo_cap = max;
            unsigned char *nb = (unsigned char *)realloc(b, novo_cap + 1);
            if (!nb) { logf_("diag ler_tudo: realloc falhou p=%s n=%zu", p, novo_cap); free(b); sceKernelClose(fd); return NULL; }
            b = nb;
            cap = novo_cap;
        }
        int r = (int)sceKernelRead(fd, b + lido, cap - lido);
        if (r < 0) { logf_("diag ler_tudo: erro de leitura p=%s r=%d lido=%zu", p, r, lido); free(b); sceKernelClose(fd); return NULL; }
        if (r == 0) break;
        lido += (size_t)r;
    }
    sceKernelClose(fd);
    if (lido == 0) { logf_("diag ler_tudo: arquivo vazio p=%s", p); free(b); return NULL; }
    size_t n = lido;
    b[n] = 0;
    *n_out = n;
    return b;
}
static int gravar_modo(const char *p, const void *d, size_t n, int modo)
{
    int fd = sceKernelOpen(p, 0x0001 | 0x0200 | 0x0400, modo);
    if (fd < 0) return -1;
    size_t w = 0;
    while (w < n) { int r = (int)sceKernelWrite(fd, (const char *)d + w, n - w); if (r <= 0) break; w += (size_t)r; }
    sceKernelClose(fd);
    return w == n ? 0 : -1;
}
static int gravar(const char *p, const void *d, size_t n)
{
    return gravar_modo(p, d, n, 0600);
}
static int nome_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 40 || s[0] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

typedef int32_t (*fn_plugin_load)(int32_t argc, const char *argv[]);

static int carregar_modulo(const char *mod, const char *titulo, const char *dev, uint64_t agora, uint64_t visto)
{
    char p[128];
    size_t n = 0;
    /* 1) licenca */
    snprintf(p, sizeof p, PASTA_LIC "/%s.lic", mod);
    unsigned char *lic = ler_tudo(p, 1024, &n);
    if (!lic) { logf_("%s: sem licenca (abra o app JK PATCHS com internet)", mod); return -1; }
    uint64_t vence = 0;
    int r = jk_lic_checar((const char *)lic, n, JK_LIC_PUB, mod, NULL, dev, agora, visto, &vence);
    free(lic);
    if (r != JK_LIC_OK) { logf_("%s: %s", mod, jk_lic_motivo(r)); return -1; }
    /* 2) chave do conteudo */
    snprintf(p, sizeof p, PASTA_LIC "/%s.key", mod);
    unsigned char *kt = ler_tudo(p, 256, &n);
    unsigned char chave[32];
    if (!kt || jkm_chave_hex((const char *)kt, n, chave)) { if (kt) free(kt); logf_("%s: sem chave do modulo (abra o app com internet)", mod); return -1; }
    free(kt);
    /* 3) modulo cifrado */
    snprintf(p, sizeof p, PASTA_CORE "/%s.jkm", mod);
    unsigned char *jkm = ler_tudo(p, 70u * 1024 * 1024, &n);
    if (!jkm) { logf_("%s: modulo nao instalado", mod); memset(chave, 0, sizeof chave); return -1; }
    jkm_info inf;
    if (jkm_ler(jkm, n, &inf) != JKM_OK || strcmp(inf.modulo, mod)) { logf_("%s: arquivo do modulo invalido", mod); free(jkm); memset(chave, 0, sizeof chave); return -1; }
    if (!jkm_titulo_ok(&inf, titulo)) { logf_("%s: nao e para este jogo", mod); free(jkm); memset(chave, 0, sizeof chave); return 0; }
    unsigned char *prx = (unsigned char *)malloc(inf.tamanho);
    if (!prx) { free(jkm); memset(chave, 0, sizeof chave); return -1; }
    r = jkm_decifrar(&inf, chave, prx);
    memset(chave, 0, sizeof chave);
    free(jkm);
    if (r != JKM_OK) { logf_("%s: chave nao confere com o modulo", mod); free(prx); return -1; }
    /* 4) temporario -> carrega -> apaga */
    uint64_t sal = (uint64_t)sceKernelGetProcessTime() ^ (uint64_t)(uintptr_t)prx;
    snprintf(p, sizeof p, PASTA_CORE "/.t%llx.prx", (unsigned long long)sal);
    /* precisa do bit de execucao (0700): sceKernelLoadStartModule recusa (rc=0x8002000d,
     * equivalente a "permissao negada") um arquivo gravado so com leitura/escrita (0600). */
    if (gravar_modo(p, prx, inf.tamanho, 0700)) { logf_("%s: falha ao preparar", mod); memset(prx, 0, inf.tamanho); free(prx); return -1; }
    memset(prx, 0, inf.tamanho);
    free(prx);
    int32_t h = (int32_t)sceKernelLoadStartModule(p, 0, 0, 0, 0, 0);
    sceKernelUnlink(p);
    if (h < 0) { logf_("%s: o sistema recusou carregar (rc=0x%08x)", mod, (uint32_t)h); return -1; }
    void *f = 0;
    if (sceKernelDlsym(h, "plugin_load", &f) != 0 || !f) { logf_("%s: carregado, mas sem plugin_load", mod); return 1; }
    int32_t rc = ((fn_plugin_load)f)(0, 0);
    logf_("%s: ativo (licenca por mais %llu h, plugin_load=%d)", mod, (unsigned long long)((vence - agora) / 3600), rc);
    return 1;
}

static void *trabalho(void *arg)
{
    (void)arg;
    sceKernelMkdir("/data/jk", 0777);
    sceKernelMkdir(PASTA_CORE, 0777);
    logf_("JK CORE v1.00");
    struct proc_info pi; memset(&pi, 0, sizeof pi);
    if (sys_sdk_proc_info(&pi) != 0) { logf_("sem informacao do jogo; nada carregado"); return 0; }
    char titulo[16]; snprintf(titulo, sizeof titulo, "%.9s", pi.titleid);
#ifdef JK_LIC_PUB_PENDENTE
    logf_("build sem chave publica (source/lic_pub.h provisorio): nada carregado");
    return 0;
#else
    size_t n = 0;
    unsigned char *dv = ler_tudo(ARQ_DEV, 80, &n);
    if (dv && n < 32) logf_("diag dev: leu arquivo mas veio curto, n=%zu", n);
    if (!dv || n < 32) { if (dv) free(dv); logf_("sem id do console: abra o app JK PATCHS uma vez"); return 0; }
    char dev[33]; memcpy(dev, dv, 32); dev[32] = 0; free(dv);
    for (int i = 0; i < 32; i++) if (dev[i] >= 'A' && dev[i] <= 'F') dev[i] = (char)(dev[i] + 32);
    OrbisKernelTimeval tv; memset(&tv, 0, sizeof tv);
    sceKernelGettimeofday(&tv);
    uint64_t agora = tv.tv_sec > 0 ? (uint64_t)tv.tv_sec : 0, visto = 0;
    unsigned char *vb = ler_tudo(ARQ_VISTO, 32, &n);
    if (vb) { for (size_t i = 0; i < n && vb[i] >= '0' && vb[i] <= '9'; i++) visto = visto * 10 + (uint64_t)(vb[i] - '0'); free(vb); }
    unsigned char *ind = ler_tudo(PASTA_LIC "/indice.txt", 4096, &n);
    if (!ind) { logf_("nenhum modulo liberado (abra o app JK PATCHS com internet)"); return 0; }
    int ativos = 0, total = 0;
    char *s = (char *)ind;
    while (s && *s && total < MAX_MODS) {
        char *nl = strchr(s, '\n');
        if (nl) *nl = 0;
        size_t l = strlen(s);
        while (l && (s[l - 1] == '\r' || s[l - 1] == ' ')) s[--l] = 0;
        if (nome_ok(s)) { total++; if (carregar_modulo(s, titulo, dev, agora, visto) > 0) ativos++; }
        s = nl ? nl + 1 : NULL;
    }
    free(ind);
    if (ativos && agora > visto) {
        char t[24]; int k = snprintf(t, sizeof t, "%llu", (unsigned long long)agora);
        if (k > 0) gravar(ARQ_VISTO, t, (size_t)k);
    }
    logf_("%d de %d modulo(s) ativo(s) em %s", ativos, total, titulo);
    if (g_log >= 0) { sceKernelClose(g_log); g_log = -1; }
    return 0;
#endif
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    scePthreadCreate(&g_thr, 0, trabalho, 0, "jk_core");
    return 0;
}
int32_t attr_public plugin_unload(int32_t argc, const char *argv[]) { (void)argc; (void)argv; return 0; }
int32_t attr_module_hidden module_start(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
int32_t attr_module_hidden module_stop(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
