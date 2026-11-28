/*
 * JK EFUTSAL SONDA v1.01 - PES 2021 (CUSA18676) - etapa 0 do eFutsal no PS4.
 * Plugin SO DE LEITURA. Nao grava nada no jogo e nao hooka nada.
 * Objetivo: descobrir no PS4 as estruturas que o mod de PC (efutsal.lua) usa:
 *   - MatchInfo (escalacao, formacao, flags de quem joga: no PC +0x288, +0x598, +0x1334, +0x1748)
 *   - bloco de geometria do campo (floats 52.5, 34.0, 3.66, 2.44 ...)
 * Como funciona (tudo automatico, thread propria; plugin_load so cria a thread):
 *   1) a cada 12 s varre a memoria RW procurando uma tabela de 20+ registros de jogador
 *      (passo 0x188, id u32 em +4, nome ASCII em +0xc). Achando, MatchInfo = tabela - 0x1334.
 *   2) loga times, ids, formacao, flags e grava mi_N.bin (32 KB) em /data/jk/efutsal.
 *   3) uma vez, depois de ver o MatchInfo 2 vezes seguidas (partida em andamento): procura o
 *      bloco de geometria do campo e quem aponta para o MatchInfo / para o bloco.
 *   4) /data/jk/efutsal/busca.txt (1 texto por linha, ate 4) -> loga onde cada texto aparece
 *      na memoria (+ contexto). O plugin apaga o arquivo depois. Serve se o layout for outro.
 * Log: /data/jk/efutsal/sonda.log
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

attr_public const char *g_pluginName = "jk_efutsal_sonda";
attr_public const char *g_pluginDesc = "JK PATCHS - sonda do eFutsal (so leitura)";
attr_public const char *g_pluginAuth = "JK PATCHS";
attr_public uint32_t g_pluginVersion = 0x00000101;

int jk_qmp(void *addr, void **start, void **end, int *prot) __asm__("sceKernelQueryMemoryProtection");

#define PASTA "/data/jk/efutsal"
#define LIMITE_VA 0x7fc0000000ULL

#define MI_REC   0x1334
#define REC_SZ   0x188
#define MI_TNAME 0x13c
#define MI_IDS   0x288
#define MI_FORM  0x598
#define MI_FLAGS 0x1748
#define MI_LADO  0x690
#define MI_DUMP  0x8000

static uint64_t g_base;
static volatile int g_rodando = 1;
static OrbisPthread g_thr;
static int g_log = -1;

static void logf_(const char *fmt, ...)
{
    char b[1200];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    if (g_log < 0) g_log = sceKernelOpen(PASTA "/sonda.log", 0x0001 | 0x0200 | 0x0008, 0666);
    if (g_log >= 0) sceKernelWrite(g_log, b, n);
}

static int existe(const char *p) { OrbisKernelStat st; return sceKernelStat(p, &st) == 0; }

/* percorre as regioes mapeadas; cb(inicio, fim, prot) devolve 1 para parar */
typedef int (*regiao_cb)(uint64_t ini, uint64_t fim, int prot, void *ud);
static void para_cada_regiao(int so_rw, regiao_cb cb, void *ud)
{
    uint64_t a = 0x10000;
    while (a < LIMITE_VA && g_rodando) {
        void *s = 0, *e = 0; int p = 0;
        if (jk_qmp((void *)a, &s, &e, &p) != 0) { a += 0x10000; continue; }
        uint64_t rs = (uint64_t)s, re = (uint64_t)e;
        if (re <= a) { a += 0x10000; continue; }
        if (rs < a) rs = a;
        int ok = so_rw ? ((p & 3) == 3) : ((p & 1) != 0);
        if (ok && (p & ~3) == 0 && re - rs >= 0x1000) { if (cb(rs, re, p, ud)) return; }
        a = re;
    }
}

static int nome_ok(const uint8_t *s)
{
    int n = 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] >= 0xc3)) return 0;
    while (n < 0x40 && s[n]) { if (s[n] < 0x20 || s[n] == 0x7f) return 0; n++; }
    return n >= 2 && n < 0x40;
}

static int rec_ok(uint64_t a)
{
    uint32_t id = *(volatile uint32_t *)(a + 4);
    if (id == 0 || id == 0xffffffffu) return 0;
    return nome_ok((const uint8_t *)(a + 0xc));
}

/* ---------- 1) MatchInfo ---------- */
static uint64_t g_mi_achado;
static int mi_cb(uint64_t ini, uint64_t fim, int prot, void *ud)
{
    (void)prot; (void)ud;
    for (uint64_t a = (ini + 7) & ~7ULL; a + 24 * REC_SZ + 0x80 < fim; a += 8) {
        uint32_t id = *(volatile uint32_t *)(a + 4);
        if (id == 0 || id == 0xffffffffu) continue;
        if (!nome_ok((const uint8_t *)(a + 0xc))) continue;
        int k;
        for (k = 1; k < 22; k++) if (!rec_ok(a + (uint64_t)k * REC_SZ)) break;
        if (k < 22) continue;
        uint64_t mi = a - MI_REC;
        if (mi < ini) continue;
        if (!nome_ok((const uint8_t *)(mi + MI_TNAME))) continue;
        g_mi_achado = mi;
        return 1;
    }
    return 0;
}

static void str_seguro(char *dst, const uint8_t *s, int max)
{
    int i = 0;
    for (; i < max - 1 && s[i]; i++) dst[i] = (s[i] >= 0x20 && s[i] < 0x7f) ? (char)s[i] : '?';
    dst[i] = 0;
}

static void hex_linha(const char *rot, uint64_t a, int n)
{
    char b[400]; int o = 0;
    o += snprintf(b + o, sizeof b - o, "%s [0x%llx]:", rot, (unsigned long long)a);
    for (int i = 0; i < n && o < (int)sizeof b - 4; i++) o += snprintf(b + o, sizeof b - o, " %02x", *(volatile uint8_t *)(a + i));
    logf_("%s", b);
}

static int g_ndump;
static void relatorio_mi(uint64_t mi)
{
    char t1[80], t2[80];
    str_seguro(t1, (const uint8_t *)(mi + MI_TNAME), sizeof t1);
    str_seguro(t2, (const uint8_t *)(mi + MI_TNAME + MI_LADO), sizeof t2);
    logf_("MATCHINFO achado em 0x%llx  (heap, base_eboot=0x%llx)  casa='%s' fora='%s'", (unsigned long long)mi, (unsigned long long)g_base, t1, t2);
    for (int s = 0; s < 2; s++) {
        uint64_t b = mi + (uint64_t)s * MI_LADO;
        char o[400]; int n = 0;
        n += snprintf(o + n, sizeof o - n, "  lado%d ids(+0x288):", s);
        for (int i = 0; i < 11; i++) n += snprintf(o + n, sizeof o - n, " %u", *(volatile uint32_t *)(b + MI_IDS + i * 8));
        logf_("%s", o);
        hex_linha("  formacao(+0x598)", b + MI_FORM, 34);
    }
    hex_linha("  flags(+0x1748)", mi + MI_FLAGS, 24);
    for (int r = 0; r < 24; r++) {
        uint64_t a = mi + MI_REC + (uint64_t)r * REC_SZ;
        char nm[72]; str_seguro(nm, (const uint8_t *)(a + 0xc), sizeof nm);
        logf_("  rec%02d id=%u nome='%s'", r, *(volatile uint32_t *)(a + 4), nm);
    }
    char nome[80]; snprintf(nome, sizeof nome, PASTA "/mi_%d.bin", g_ndump);
    int fd = sceKernelOpen(nome, 0x0001 | 0x0200 | 0x0400, 0666);
    if (fd >= 0) { sceKernelWrite(fd, (void *)mi, MI_DUMP); sceKernelClose(fd); logf_("  gravado %s (%d bytes)", nome, MI_DUMP); }
    g_ndump++;
}

/* ---------- 2) quem aponta para um endereco ---------- */
typedef struct { uint64_t alvo; int n, max; } PtrBusca;
static int ptr_cb(uint64_t ini, uint64_t fim, int prot, void *ud)
{
    (void)prot;
    PtrBusca *pb = (PtrBusca *)ud;
    for (uint64_t a = (ini + 7) & ~7ULL; a + 8 <= fim; a += 8) {
        if (*(volatile uint64_t *)a != pb->alvo) continue;
        if (a >= g_base && a < g_base + 0x6200000ULL)
            logf_("  ponteiro p/ 0x%llx guardado em 0x%llx  <-- DENTRO DO EBOOT rva=0x%llx", (unsigned long long)pb->alvo, (unsigned long long)a, (unsigned long long)(a - g_base));
        else
            logf_("  ponteiro p/ 0x%llx guardado em 0x%llx", (unsigned long long)pb->alvo, (unsigned long long)a);
        if (++pb->n >= pb->max) return 1;
    }
    return 0;
}
static void quem_aponta(uint64_t alvo, int max)
{
    PtrBusca pb; pb.alvo = alvo; pb.n = 0; pb.max = max;
    para_cada_regiao(1, ptr_cb, &pb);
    if (!pb.n) logf_("  ninguem aponta para 0x%llx nas regioes RW", (unsigned long long)alvo);
}

/* ---------- 3) bloco de geometria do campo ---------- */
static uint64_t g_pitch[32]; static int g_npitch, g_pitch_rw[32];
static int in_r(float v, float lo, float hi) { return v >= lo && v <= hi; }
static int pitch_cb(uint64_t ini, uint64_t fim, int prot, void *ud)
{
    (void)ud;
    for (uint64_t a = (ini + 3) & ~3ULL; a + 64 <= fim; a += 4) {
        volatile float *f = (volatile float *)a;
        if (!in_r(f[0], 52.4f, 52.7f)) continue;
        if (!in_r(f[1], 33.9f, 34.2f) || !in_r(f[2], 3.5f, 3.8f) || !in_r(f[3], 2.3f, 2.6f)) continue;
        if (g_npitch < 32) { g_pitch[g_npitch] = a; g_pitch_rw[g_npitch] = ((prot & 3) == 3); g_npitch++; }
    }
    return 0;
}
static void relatorio_pitch(void)
{
    g_npitch = 0;
    para_cada_regiao(0, pitch_cb, 0);
    logf_("GEOMETRIA: %d bloco(s) com floats 52.5/34.0/3.66/2.44 em sequencia", g_npitch);
    for (int i = 0; i < g_npitch; i++) {
        uint64_t a = g_pitch[i]; char o[500]; int n = 0;
        n += snprintf(o + n, sizeof o - n, "  bloco 0x%llx %s%s floats:", (unsigned long long)a, g_pitch_rw[i] ? "RW" : "RO",
                      (a >= g_base && a < g_base + 0x6200000ULL) ? " (EBOOT rva=?)" : "");
        for (int k = 0; k < 18; k++) n += snprintf(o + n, sizeof o - n, " %.3f", (double)*(volatile float *)(a + k * 4));
        logf_("%s", o);
        if (a >= g_base && a < g_base + 0x6200000ULL) logf_("     rva=0x%llx", (unsigned long long)(a - g_base));
    }
    int feitos = 0;
    for (int i = 0; i < g_npitch && feitos < 3; i++) if (g_pitch_rw[i]) { logf_("PONTEIROS para bloco 0x%llx:", (unsigned long long)g_pitch[i]); quem_aponta(g_pitch[i], 12); feitos++; }
}

/* ---------- 4) busca de texto ---------- */
typedef struct { const uint8_t *pat; int len, n; uint64_t ex_lo, ex_hi; const char *rot; } TxtBusca;
static int txt_cb(uint64_t ini, uint64_t fim, int prot, void *ud)
{
    (void)prot;
    TxtBusca *tb = (TxtBusca *)ud;
    for (uint64_t a = ini; a + tb->len <= fim; a++) {
        if (*(volatile uint8_t *)a != tb->pat[0]) continue;
        if (a >= tb->ex_lo && a < tb->ex_hi) continue;          /* buffers do proprio plugin */
        if (memcmp((const void *)a, tb->pat, tb->len) != 0) continue;
        logf_("  [%s] em 0x%llx", tb->rot, (unsigned long long)a);
        hex_linha("     contexto-8", a - 8, 0x48);
        if (++tb->n >= 10) return 1;
    }
    return 0;
}
static int utf8_para_utf16(const char *s, int n, uint8_t *o, int max)
{
    int k = 0;
    for (int i = 0; i < n && k + 2 <= max; ) {
        uint32_t c = (uint8_t)s[i];
        if (c < 0x80) i++;
        else if ((c & 0xe0) == 0xc0 && i + 1 < n) { c = ((c & 0x1f) << 6) | ((uint8_t)s[i + 1] & 0x3f); i += 2; }
        else { c = '?'; i++; }
        o[k++] = (uint8_t)(c & 0xff); o[k++] = (uint8_t)(c >> 8);
    }
    return k;
}
static void busca_variantes(const char *txt, int len, uint64_t ex_lo, uint64_t ex_hi)
{
    uint8_t v[256]; TxtBusca tb; tb.ex_lo = ex_lo; tb.ex_hi = ex_hi;
    if (len <= 0 || len > 100) return;
    logf_("BUSCA '%s':", txt);
    tb.pat = (const uint8_t *)txt; tb.len = len; tb.n = 0; tb.rot = "ascii/utf8";
    para_cada_regiao(1, txt_cb, &tb);
    int u = utf8_para_utf16(txt, len, v, sizeof v);
    tb.pat = v; tb.len = u; tb.n = 0; tb.rot = "utf16";
    para_cada_regiao(1, txt_cb, &tb);
    uint8_t up[128]; int ok = 0;
    for (int i = 0; i < len; i++) { uint8_t c = (uint8_t)txt[i]; if (c >= 'a' && c <= 'z') { c -= 32; ok = 1; } up[i] = c; }
    if (ok) { tb.pat = up; tb.len = len; tb.n = 0; tb.rot = "MAIUSCULAS"; para_cada_regiao(1, txt_cb, &tb); }
}
static void busca_textos(void)
{
    char buf[512]; int fd = sceKernelOpen(PASTA "/busca.txt", 0, 0);
    if (fd < 0) return;
    int n = (int)sceKernelRead(fd, buf, sizeof buf - 1); sceKernelClose(fd);
    if (n <= 0) { sceKernelUnlink(PASTA "/busca.txt"); return; }
    buf[n] = 0;
    uint64_t ex_lo = (uint64_t)(uintptr_t)buf - 0x40000, ex_hi = (uint64_t)(uintptr_t)buf + 0x40000;
    char *p = buf; int lin = 0;
    while (*p && lin < 4) {
        char *e = p; while (*e && *e != '\n' && *e != '\r') e++;
        char sv = *e; *e = 0;
        if (e > p) { busca_variantes(p, (int)(e - p), ex_lo, ex_hi); lin++; }
        *e = sv; p = e; while (*p == '\n' || *p == '\r') p++;
    }
    sceKernelUnlink(PASTA "/busca.txt");
}

static void *trabalho(void *arg)
{
    (void)arg;
    sceKernelMkdir("/data/jk", 0777);
    sceKernelMkdir(PASTA, 0777);
    struct proc_info pi; memset(&pi, 0, sizeof pi);
    if (sys_sdk_proc_info(&pi) != 0) { logf_("ERRO proc_info"); return 0; }
    g_base = pi.base_address;
    logf_("JK_EFUTSAL_SONDA v1.01 titulo=%s versao=%s base=0x%llx", pi.titleid, pi.version, (unsigned long long)g_base);
    sceKernelUsleep(30000000);
    uint64_t ultimo = 0; int vezes = 0, pitch_feito = 0;
    logf_("Aguardando partida. Quando o MatchInfo for achado 2x seguidas o relatorio completo sai sozinho (pesado: ~3 s de varredura). Para repetir: crie /data/jk/efutsal/repetir.txt");
    while (g_rodando) {
        busca_textos();
        if (existe(PASTA "/repetir.txt")) { sceKernelUnlink(PASTA "/repetir.txt"); pitch_feito = 0; ultimo = 0; vezes = 0; g_ndump = 0; logf_("REPETIR pedido"); }
        if (pitch_feito || g_ndump >= 6) { sceKernelUsleep(5000000); continue; }
        g_mi_achado = 0;
        para_cada_regiao(1, mi_cb, 0);
        if (g_mi_achado) {
            if (g_mi_achado == ultimo) vezes++; else { ultimo = g_mi_achado; vezes = 1; relatorio_mi(ultimo); logf_("PONTEIROS para MatchInfo 0x%llx:", (unsigned long long)ultimo); quem_aponta(ultimo, 12); }
            if (vezes >= 2 && !pitch_feito) { relatorio_pitch(); pitch_feito = 1; logf_("RELATORIO COMPLETO - pode mandar sonda.log e mi_0.bin"); }
        } else vezes = 0;
        sceKernelUsleep(20000000);
    }
    logf_("FIM (dumps=%d)", g_ndump);
    return 0;
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    scePthreadCreate(&g_thr, 0, trabalho, 0, "jk_efutsal_sonda");
    return 0;
}
int32_t attr_public plugin_unload(int32_t argc, const char *argv[]) { (void)argc; (void)argv; g_rodando = 0; return 0; }
int32_t attr_module_hidden module_start(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
int32_t attr_module_hidden module_stop(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
