/*
 * JK APROBE - sonda de memoria (estilo "Cheat Engine") para achar os campos da partida
 * no PES 2021 CUSA22062. NAO altera nada no jogo: so le memoria e escreve um log.
 *
 * Como usar (pelo FTP, porta 2121):
 *   1. Instale o jk_aprobe.prx no GoldHEN e abra o jogo. Entre numa partida.
 *   2. Crie o arquivo  /data/jk/aprobe/cmd.txt  com UM comando por linha (pode ser varias).
 *      O plugin le o arquivo em ate 1 s, executa e apaga. O resultado vai para
 *      /data/jk/aprobe/aprobe.log  (baixe esse arquivo e me mande).
 *
 * Comandos (valores decimais; enderecos em hexa):
 *   info                         base do jogo + regioes de memoria (resumo)
 *   new i32 N                    primeira busca: todo int32 == N na memoria gravavel
 *   new f32 X [eps]              primeira busca: floats iguais a X (eps padrao 0.01)
 *   new u8 N                     primeira busca: bytes == N
 *   next i32 N | f32 X [eps] | u8 N     filtra os candidatos pelo valor atual
 *   next chg | same | inc | dec         filtra por mudou / igual / subiu / desceu desde a ultima busca
 *   list [N]                     mostra os N primeiros candidatos (padrao 40)
 *   ptr ADDR [voltar]            acha quem guarda um ponteiro para [ADDR-voltar, ADDR] (padrao 0x2000)
 *   dump ADDR LEN                hexdump (LEN ate 4096)
 *   rd ADDR                      mostra o valor em ADDR como i32, f32 e u8
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>
#include "GoldHEN.h"

#define attr_public __attribute__((visibility("default")))
#define attr_module_hidden __attribute__((weak)) __attribute__((visibility("hidden")))

attr_public const char *g_pluginName = "jk_aprobe";
attr_public const char *g_pluginDesc = "JK PATCHS - sonda de memoria (diagnostico)";
attr_public const char *g_pluginAuth = "JK PATCHS";
attr_public uint32_t g_pluginVersion = 0x00000100;

#define PASTA "/data/jk/aprobe"
#define MAXC  131072   /* estatico: o malloc do jogo nao tem memoria sobrando */

static OrbisPthread g_thr;
static volatile int g_running = 1;
static int g_log = -1;
static uint64_t g_base = 0;

static uint64_t c_addr[MAXC];    /* candidatos */
static uint32_t c_old[MAXC];     /* valor anterior (4 bytes; u8 usa o byte baixo) */
static int c_n = 0, c_kind = 0;  /* 1=i32 2=f32 3=u8 */
static int c_over = 0;

int jk_vq(const void *addr, int flags, void *info, size_t sz) __asm__("sceKernelVirtualQuery");

static void logf_(const char *fmt, ...)
{
    char b[600];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    if (g_log < 0) g_log = sceKernelOpen(PASTA "/aprobe.log", 0x0001 | 0x0200 | 0x0008, 0666);
    if (g_log >= 0) sceKernelWrite(g_log, b, n);
}

/* Regiao gravavel seguinte a partir de 'from'. Layout de SceKernelVirtualQueryInfo:
   start(8) end(8) offset(8) protection(4) memoryType(4) bits(4) name[32]. */
static int proxima_regiao(uint64_t from, uint64_t *s, uint64_t *e, int *prot, int *bits)
{
    unsigned char info[160]; memset(info, 0, sizeof info);
    if (jk_vq((const void *)from, 1 /* FIND_NEXT */, info, sizeof info) != 0) return 0;
    *s = *(uint64_t *)(info + 0);
    *e = *(uint64_t *)(info + 8);
    *prot = *(int *)(info + 24);
    *bits = *(int *)(info + 32);
    return *e > *s;
}

#define VALIDA(p, b) (((p) & 3) == 3 && !((b) & 4) /* nao pilha */ )

static int val_ok(int kind, uint32_t cur, int32_t iv, float fv, float eps)
{
    if (kind == 1) return (int32_t)cur == iv;
    if (kind == 3) return (cur & 0xff) == (uint32_t)(iv & 0xff);
    float f; memcpy(&f, &cur, 4);
    float d = f - fv; if (d < 0) d = -d;
    return d <= eps;
}

static void nova_busca(int kind, int32_t iv, float fv, float eps)
{
    c_n = 0; c_kind = kind; c_over = 0;
    uint64_t a = 0x10000, s, e; int p, bt;
    int step = (kind == 3) ? 1 : 4;
    while (proxima_regiao(a, &s, &e, &p, &bt)) {
        a = e;
        if (!VALIDA(p, bt)) continue;
        if (s < 0x10000) continue;
        for (uint64_t q = s; q + 4 <= e; q += step) {
            uint32_t cur = (kind == 3) ? *(volatile uint8_t *)q : *(volatile uint32_t *)q;
            if (val_ok(kind, cur, iv, fv, eps)) {
                if (c_n < MAXC) { c_addr[c_n] = q; c_old[c_n] = cur; c_n++; } else c_over++;
            }
        }
    }
    logf_("new: %d candidatos%s", c_n, c_over ? " (LIMITE ATINGIDO: use um valor mais raro)" : "");
}

static int regiao_legivel(uint64_t q)
{
    uint64_t s, e; int p, bt;
    if (!proxima_regiao(q, &s, &e, &p, &bt)) return 0;
    return s <= q && q < e && (p & 1);
}

static void filtra(const char *modo, int32_t iv, float fv, float eps)
{
    int k = 0;
    for (int i = 0; i < c_n; i++) {
        uint64_t q = c_addr[i];
        if (!regiao_legivel(q)) continue;
        uint32_t cur = (c_kind == 3) ? *(volatile uint8_t *)q : *(volatile uint32_t *)q;
        int ok = 0;
        if (!strcmp(modo, "val")) ok = val_ok(c_kind, cur, iv, fv, eps);
        else if (!strcmp(modo, "chg")) ok = cur != c_old[i];
        else if (!strcmp(modo, "same")) ok = cur == c_old[i];
        else if (!strcmp(modo, "inc")) {
            if (c_kind == 2) { float a, b; memcpy(&a, &cur, 4); memcpy(&b, &c_old[i], 4); ok = a > b; }
            else ok = (int32_t)cur > (int32_t)c_old[i];
        } else if (!strcmp(modo, "dec")) {
            if (c_kind == 2) { float a, b; memcpy(&a, &cur, 4); memcpy(&b, &c_old[i], 4); ok = a < b; }
            else ok = (int32_t)cur < (int32_t)c_old[i];
        }
        if (ok) { c_addr[k] = q; c_old[k] = cur; k++; }
    }
    c_n = k;
    logf_("next %s: %d candidatos", modo, c_n);
}

static void lista(int n)
{
    if (n > c_n) n = c_n;
    for (int i = 0; i < n; i++) {
        uint64_t q = c_addr[i]; uint32_t cur = *(volatile uint32_t *)q; float f; memcpy(&f, &cur, 4);
        logf_("  0x%llx  i32=%d  f32=%g  rel_base=0x%llx", (unsigned long long)q, (int32_t)cur, f,
              (unsigned long long)(q - g_base));
    }
    logf_("list: %d de %d", n, c_n);
}

static void acha_ponteiros(uint64_t alvo, uint64_t voltar)
{
    uint64_t lo = alvo > voltar ? alvo - voltar : 0, hi = alvo;
    uint64_t a = 0x10000, s, e; int p, bt, n = 0;
    while (proxima_regiao(a, &s, &e, &p, &bt) && n < 200) {
        a = e;
        if (!VALIDA(p, bt) || s < 0x10000) continue;
        for (uint64_t q = (s + 7) & ~7ULL; q + 8 <= e; q += 8) {
            uint64_t v = *(volatile uint64_t *)q;
            if (v >= lo && v <= hi) {
                logf_("  ptr em 0x%llx (rel_base=0x%llx) -> 0x%llx  (alvo-v=0x%llx)", (unsigned long long)q,
                      (unsigned long long)(q - g_base), (unsigned long long)v, (unsigned long long)(alvo - v));
                if (++n >= 200) break;
            }
        }
    }
    logf_("ptr: %d resultados", n);
}

static void dump(uint64_t a, uint64_t len)
{
    if (len > 4096) len = 4096;
    if (!regiao_legivel(a)) { logf_("dump: 0x%llx nao legivel", (unsigned long long)a); return; }
    char ln[200];
    for (uint64_t o = 0; o < len; o += 16) {
        int k = snprintf(ln, sizeof ln, "%llx:", (unsigned long long)(a + o));
        for (int j = 0; j < 16 && o + j < len; j++)
            k += snprintf(ln + k, sizeof ln - k, " %02x", *(volatile uint8_t *)(a + o + j));
        logf_("%s", ln);
    }
}

static void info(void)
{
    logf_("base=0x%llx", (unsigned long long)g_base);
    uint64_t a = 0x10000, s, e; int p, bt, n = 0; unsigned long long tot = 0;
    while (proxima_regiao(a, &s, &e, &p, &bt)) {
        a = e;
        if (VALIDA(p, bt)) { tot += e - s; if (n++ < 60) logf_("  rw 0x%llx-0x%llx (%lluKB)", (unsigned long long)s, (unsigned long long)e, (unsigned long long)((e - s) >> 10)); }
    }
    logf_("regioes gravaveis: %d, total %llu MB", n, tot >> 20);
}

static void roda_linha(char *ln)
{
    char c[16] = "", t[16] = "", x[40] = "", y[40] = "";
    int nn = sscanf(ln, "%15s %15s %39s %39s", c, t, x, y);
    if (nn < 1) return;
    logf_(">> %s", ln);
    if (!strcmp(c, "info")) { info(); return; }
    if (!strcmp(c, "list")) { lista(nn >= 2 ? atoi(t) : 40); return; }
    if (!strcmp(c, "ptr") && nn >= 2) { acha_ponteiros(strtoull(t, 0, 16), nn >= 3 ? strtoull(x, 0, 16) : 0x2000); return; }
    if (!strcmp(c, "dump") && nn >= 3) { dump(strtoull(t, 0, 16), strtoull(x, 0, 0)); return; }
    if (!strcmp(c, "rd") && nn >= 2) {
        uint64_t q = strtoull(t, 0, 16);
        if (!regiao_legivel(q)) { logf_("rd: nao legivel"); return; }
        uint32_t v = *(volatile uint32_t *)q; float f; memcpy(&f, &v, 4);
        logf_("  0x%llx i32=%d f32=%g u8=%u", (unsigned long long)q, (int32_t)v, f, v & 0xff);
        return;
    }
    int isnew = !strcmp(c, "new"), isnext = !strcmp(c, "next");
    if (!isnew && !isnext) { logf_("comando desconhecido"); return; }
    int kind = !strcmp(t, "i32") ? 1 : !strcmp(t, "f32") ? 2 : !strcmp(t, "u8") ? 3 : 0;
    if (isnew && kind) {
        float eps = (kind == 2 && nn >= 4) ? (float)atof(y) : 0.01f;
        nova_busca(kind, kind == 2 ? 0 : (int32_t)atoi(x), kind == 2 ? (float)atof(x) : 0, eps);
    } else if (isnext && kind) {
        if (kind != c_kind) { logf_("tipo diferente da busca atual"); return; }
        float eps = (kind == 2 && nn >= 4) ? (float)atof(y) : 0.01f;
        filtra("val", kind == 2 ? 0 : (int32_t)atoi(x), kind == 2 ? (float)atof(x) : 0, eps);
    } else if (isnext && (!strcmp(t, "chg") || !strcmp(t, "same") || !strcmp(t, "inc") || !strcmp(t, "dec"))) {
        filtra(t, 0, 0, 0);
    } else logf_("argumentos invalidos");
}

static void *trabalho(void *arg)
{
    (void)arg;
    sceKernelMkdir("/data/jk", 0777);
    sceKernelMkdir(PASTA, 0777);
    struct proc_info pi; memset(&pi, 0, sizeof pi);
    if (sys_sdk_proc_info(&pi) == 0) g_base = pi.base_address;
    logf_("JK_APROBE iniciado. titulo=%s base=0x%llx", pi.titleid, (unsigned long long)g_base);
    while (g_running) {
        sceKernelUsleep(1000000);
        int fd = sceKernelOpen(PASTA "/cmd.txt", 0, 0);
        if (fd < 0) continue;
        char buf[2048]; memset(buf, 0, sizeof buf);
        int n = (int)sceKernelRead(fd, buf, sizeof buf - 1);
        sceKernelClose(fd);
        sceKernelUnlink(PASTA "/cmd.txt");
        if (n <= 0) continue;
        for (char *l = buf; *l;) {
            char *eol = l; while (*eol && *eol != '\n' && *eol != '\r') eol++;
            char sv = *eol; *eol = 0;
            if (*l) roda_linha(l);
            if (!sv) break;
            l = eol + 1;
        }
        logf_("-- pronto --");
    }
    return 0;
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    scePthreadCreate(&g_thr, 0, trabalho, 0, "jk_aprobe");
    return 0;
}
int32_t attr_public plugin_unload(int32_t argc, const char *argv[]) { (void)argc; (void)argv; g_running = 0; return 0; }
int32_t attr_module_hidden module_start(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
int32_t attr_module_hidden module_stop(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
