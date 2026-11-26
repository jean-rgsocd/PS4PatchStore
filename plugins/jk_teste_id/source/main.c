/*
 * JK TESTE ID - plugin GoldHEN SO DE LEITURA (fase D3: identificador real do console).
 *
 * Pergunta: quais identificadores PERSISTENTES do PS4 um plugin consegue ler dentro do jogo?
 *   1) OpenPSID (sceKernelGetOpenPsId / sceKernelGetOpenPsIdForSystem, 16 bytes)
 *   2) MAC da placa de rede (sceNetGetMacAddress, 6 bytes)
 * Hoje o app usa um numero ALEATORIO em /data/jkpatchs_dev.txt (copiavel para outro console).
 *
 * Nao mexe no jogo. As funcoes sao procuradas em tempo de execucao (sceKernelDlsym) para o
 * build nunca falhar por simbolo ausente. Grava /data/jk/teste_id.log com:
 *   - o resultado de cada tentativa (rc em hexa)
 *   - o valor MASCARADO (primeiros e ultimos 2 bytes) + uma impressao digital curta (FNV-1a)
 *     -> rode em 2 aberturas do jogo (e, se puder, em 2 consoles) e compare as impressoes.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <orbis/libkernel.h>
#include "GoldHEN.h"

#define attr_public __attribute__((visibility("default")))
#define attr_module_hidden __attribute__((weak)) __attribute__((visibility("hidden")))

attr_public const char *g_pluginName = "jk_teste_id";
attr_public const char *g_pluginDesc = "JK PATCHS - teste do ID do console (so leitura)";
attr_public const char *g_pluginAuth = "JK PATCHS";
attr_public uint32_t g_pluginVersion = 0x00000100;

#define LOG "/data/jk/teste_id.log"
static int g_log = -1;
static OrbisPthread g_thr;

static void logf_(const char *fmt, ...)
{
    char b[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    if (g_log < 0) g_log = sceKernelOpen(LOG, 0x0001 | 0x0200 | 0x0008, 0666);
    if (g_log >= 0) sceKernelWrite(g_log, b, (size_t)n);
}

static uint32_t fnv(const uint8_t *p, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
static void mostra(const char *nome, const uint8_t *p, int n)
{
    int zeros = 1;
    for (int i = 0; i < n; i++) if (p[i]) zeros = 0;
    if (zeros) { logf_("  %s: tudo zero (nao serve)", nome); return; }
    logf_("  %s: %02x%02x...%02x%02x (%d bytes) impressao=%08x", nome, p[0], p[1], p[n - 2], p[n - 1], n, fnv(p, n));
}

typedef int32_t (*fn_psid)(void *out);
typedef int32_t (*fn_mac)(void *out, int32_t flags);

static void tenta_psid(int32_t h, const char *rotulo)
{
    static const char *nomes[2] = { "sceKernelGetOpenPsId", "sceKernelGetOpenPsIdForSystem" };
    for (int k = 0; k < 2; k++) {
        void *f = 0;
        int32_t r = sceKernelDlsym(h, nomes[k], &f);
        if (r != 0 || !f) { logf_("%s %s: dlsym rc=0x%08x", rotulo, nomes[k], (uint32_t)r); continue; }
        uint8_t id[32]; memset(id, 0, sizeof id);
        int32_t rc = ((fn_psid)f)(id);
        logf_("%s %s: rc=0x%08x", rotulo, nomes[k], (uint32_t)rc);
        if (rc == 0) mostra("OpenPSID", id, 16);
    }
}

static void *trabalho(void *arg)
{
    (void)arg;
    sceKernelMkdir("/data/jk", 0777);
    sceKernelUsleep(5000000);                         /* deixa o jogo subir (rede e modulos carregados) */
    logf_("==== JK TESTE ID v1.00 ====");
    /* 1) OpenPSID: handles comuns do libkernel + o handle devolvido pelo carregador */
    int32_t hk = (int32_t)sceKernelLoadStartModule("libkernel.sprx", 0, 0, 0, 0, 0);
    logf_("libkernel.sprx handle=0x%08x", (uint32_t)hk);
    if (hk > 0) tenta_psid(hk, "[libkernel]");
    tenta_psid(0x2001, "[0x2001]");
    int32_t hks = (int32_t)sceKernelLoadStartModule("libkernel_sys.sprx", 0, 0, 0, 0, 0);
    logf_("libkernel_sys.sprx handle=0x%08x", (uint32_t)hks);
    if (hks > 0) tenta_psid(hks, "[libkernel_sys]");
    /* 2) MAC */
    int32_t hn = (int32_t)sceKernelLoadStartModule("libSceNet.sprx", 0, 0, 0, 0, 0);
    logf_("libSceNet.sprx handle=0x%08x", (uint32_t)hn);
    if (hn > 0) {
        void *f = 0;
        int32_t r = sceKernelDlsym(hn, "sceNetGetMacAddress", &f);
        if (r != 0 || !f) logf_("sceNetGetMacAddress: dlsym rc=0x%08x", (uint32_t)r);
        else {
            uint8_t mac[8]; memset(mac, 0, sizeof mac);
            int32_t rc = ((fn_mac)f)(mac, 0);
            logf_("sceNetGetMacAddress: rc=0x%08x", (uint32_t)rc);
            if (rc == 0) mostra("MAC", mac, 6);
        }
    }
    /* 3) o id atual do app (aleatorio) */
    OrbisKernelStat st;
    logf_("/data/jkpatchs_dev.txt (id aleatorio do app): %s", sceKernelStat("/data/jkpatchs_dev.txt", &st) == 0 ? "existe" : "nao existe");
    logf_("==== fim. Mande o /data/jk/teste_id.log ====");
    if (g_log >= 0) { sceKernelClose(g_log); g_log = -1; }
    return 0;
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    scePthreadCreate(&g_thr, 0, trabalho, 0, "jk_teste_id");
    return 0;
}
int32_t attr_public plugin_unload(int32_t argc, const char *argv[]) { (void)argc; (void)argv; return 0; }
int32_t attr_module_hidden module_start(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
int32_t attr_module_hidden module_stop(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
