/*
 * JK SONDA (Supercopa/Recopa - etapa 2)
 * Plugin SO DE LEITURA para PES 2021 (CUSA18676 / Mega e Brasfoot).
 * Nao grava nada no jogo. So le a Master League carregada e grava em /data/jk/super:
 *   sonda.log      -> resumo (data, clube do usuario, tabela de competicoes e inscritos)
 *   ml_N.bin       -> copia crua do bloco da carreira (para analise no PC)
 * Copia automatica 1x quando a ML termina de carregar. Para pedir outra copia,
 * crie o arquivo /data/jk/super/copiar.txt (o plugin apaga depois de copiar).
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

attr_public const char *g_pluginName = "jk_sonda";
attr_public const char *g_pluginDesc = "JK PATCHS - sonda da Master League (so leitura)";
attr_public const char *g_pluginAuth = "JK PATCHS";
attr_public uint32_t g_pluginVersion = 0x00000200;

/* nome proprio para nao conflitar com a declaracao do toolchain */
int jk_qmp(void *addr, void **start, void **end, int *prot) __asm__("sceKernelQueryMemoryProtection");

/* Enderecos (eboot 01.08 Mega/Brasfoot, mesmos usados pelo plugin da Sula 4.1) */
#define RVA_CARREIRA_PTR   0x5b4e398ULL   /* *(base+RVA) -> obj; *(obj+0x58) -> bloco da carreira */
#define RVA_TAB_COPAS      0x49591b0ULL   /* tabela do eboot: 62 linhas x 16 bytes */
#define DB_TAMANHO         0x1db7000ULL
#define DB_COMP_TAB        0x1163cc4ULL   /* competicoes: 0x388 bytes cada */
#define DB_COMP_N          0x1273364ULL   /* u32 quantidade (<=300) */
#define DB_COMP_STRIDE     0x388
#define DB_DATA            0x1baa094ULL
#define DB_CLUBE_USUARIO   0x1cef14cULL

#define PASTA "/data/jk/super"

static uint64_t g_base;
static volatile int g_rodando = 1;
static OrbisPthread g_thr;

static int legivel(uint64_t a, uint64_t n)
{
    if (a < 0x10000 || n > ~a) return 0;
    uint64_t fim = a + n;
    while (a < fim) {
        void *s = 0, *e = 0; int p = 0;
        if (jk_qmp((void *)a, &s, &e, &p) != 0) return 0;
        if ((p & 1) == 0) return 0;
        if ((uint64_t)e <= a) return 0;
        a = (uint64_t)e;
    }
    return 1;
}

static uint64_t ler_ptr(uint64_t a) { return legivel(a, 8) ? *(volatile uint64_t *)a : 0; }

static int g_log = -1;
static void logf_(const char *fmt, ...)
{
    char b[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    if (g_log < 0) g_log = sceKernelOpen(PASTA "/sonda.log", 0x0001 | 0x0200 | 0x0008, 0666); /* WRONLY|CREAT|APPEND */
    if (g_log >= 0) sceKernelWrite(g_log, b, n);
}

static int existe(const char *p)
{
    OrbisKernelStat st;
    return sceKernelStat(p, &st) == 0;
}

static uint64_t carreira(void)
{
    uint64_t obj = ler_ptr(g_base + RVA_CARREIRA_PTR);
    if (!obj) return 0;
    uint64_t db = ler_ptr(obj + 0x58);
    if (!db || !legivel(db, DB_TAMANHO)) return 0;
    uint32_t n = *(volatile uint32_t *)(db + DB_COMP_N);
    if (n == 0 || n > 300) return 0;
    return db;
}

static void resumo(uint64_t db, int num)
{
    uint8_t *d = (uint8_t *)db;
    uint32_t n = *(uint32_t *)(d + DB_COMP_N);
    logf_("==== COPIA %d  db=0x%llx  base=0x%llx", num, (unsigned long long)db, (unsigned long long)g_base);
    logf_("DATA u16: %u %u %u %u  | +0x10: %u +0x14: %u",
          *(uint16_t *)(d + DB_DATA), *(uint16_t *)(d + DB_DATA + 2),
          *(uint16_t *)(d + DB_DATA + 4), *(uint16_t *)(d + DB_DATA + 6),
          *(uint32_t *)(d + DB_DATA + 0x10), *(uint32_t *)(d + DB_DATA + 0x14));
    logf_("CLUBE_USUARIO %u", *(uint32_t *)(d + DB_CLUBE_USUARIO));
    logf_("COMPETICOES %u", n);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *c = d + DB_COMP_TAB + (uint64_t)i * DB_COMP_STRIDE;
        uint16_t key = *(uint16_t *)c;
        uint16_t e8 = *(uint16_t *)(c + 0xe8);
        uint32_t q = c[0x37e] & 0x7f; if (q > 48) q = 48;
        char lin[900]; int p = 0;
        p += snprintf(lin + p, sizeof lin - p, "C%u key=0x%x tipo=%u fase=%u e8=0x%x n=%u :", i, key, key & 0x3ff, key >> 10, e8, q);
        for (uint32_t k = 0; k < q && p < (int)sizeof lin - 16; k++) {
            uint32_t v = *(uint32_t *)(c + 0x1e4 + k * 4);
            p += snprintf(lin + p, sizeof lin - p, " %u", (v >> 14) & 0x1ffff);
        }
        logf_("%s", lin);
    }
    uint8_t *t = (uint8_t *)(g_base + RVA_TAB_COPAS);
    if (legivel((uint64_t)t, 62 * 16)) {
        for (int i = 0; i < 62; i++) {
            uint32_t *w = (uint32_t *)(t + i * 16);
            logf_("EBOOT_COPA %d: %08x %08x %08x %08x", i, w[0], w[1], w[2], w[3]);
        }
    }
}

static void copiar(uint64_t db, int num)
{
    char nome[96];
    snprintf(nome, sizeof nome, PASTA "/ml_%d.bin", num);
    int fd = sceKernelOpen(nome, 0x0001 | 0x0200 | 0x0400, 0666); /* WRONLY|CREAT|TRUNC */
    if (fd < 0) { logf_("ERRO abrir %s 0x%x", nome, fd); return; }
    uint64_t feito = 0;
    while (feito < DB_TAMANHO) {
        uint64_t n = DB_TAMANHO - feito; if (n > 0x100000) n = 0x100000;
        int w = sceKernelWrite(fd, (void *)(db + feito), n);
        if (w <= 0) { logf_("ERRO gravar %s em 0x%llx", nome, (unsigned long long)feito); break; }
        feito += (uint64_t)w;
    }
    sceKernelClose(fd);
    logf_("COPIA_GRAVADA %s bytes=%llu", nome, (unsigned long long)feito);
}


/* ---------- v2.00: copia do executavel (qualquer titulo) e assinaturas ---------- */
static int ler_k(uint64_t src, void *dest, uint64_t n)
{
    struct proc_rw rw; memset(&rw, 0, sizeof rw);
    rw.address = src; rw.data = dest; rw.length = n; rw.write_flags = 0;
    return sys_sdk_proc_rw(&rw) == 0;
}
static void hexlog(const char *nome, uint64_t rva, int n)
{
    uint8_t b[32] = {0}; char o[128]; int p = 0;
    int ok = ler_k(g_base + rva, b, n);
    for (int i = 0; i < n; i++) p += snprintf(o + p, sizeof o - p, "%02x ", b[i]);
    logf_("ASSINATURA %s rva=0x%llx %s: %s", nome, (unsigned long long)rva, ok ? "ok" : "falhou", o);
}
static uint8_t g_buf[0x100000];
static void copiar_executavel(const char *titulo)
{
    char nome[96], marca[96];
    snprintf(nome, sizeof nome, PASTA "/eboot_%.9s.bin", titulo);
    snprintf(marca, sizeof marca, PASTA "/eboot_%.9s.ok", titulo);
    if (existe(marca)) { logf_("EBOOT copia ja existe (%s); apague o .ok para copiar de novo", nome); return; }
    /* mapa das regioes a partir da base */
    uint64_t a = g_base, fim = g_base; int nreg = 0;
    while (nreg < 128 && a < g_base + 0x10000000ULL) {
        void *s = 0, *e = 0; int pr = 0;
        if (jk_qmp((void *)a, &s, &e, &pr) != 0) break;
        if ((uint64_t)s > a + 0x4000000ULL) break;          /* buraco grande: fim do modulo */
        logf_("REGIAO %d inicio=+0x%llx fim=+0x%llx prot=%d", nreg, (unsigned long long)((uint64_t)s - g_base),
              (unsigned long long)((uint64_t)e - g_base), pr);
        if ((uint64_t)e <= a) break;
        fim = (uint64_t)e; a = (uint64_t)e; nreg++;
    }
    uint64_t total = fim - g_base;
    if (total > 0xC000000ULL) total = 0xC000000ULL;          /* teto 192 MB */
    int fd = sceKernelOpen(nome, 0x0001 | 0x0200 | 0x0400, 0666);
    if (fd < 0) { logf_("ERRO abrir %s 0x%x", nome, fd); return; }
    uint64_t feito = 0, falhas = 0;
    while (feito < total) {
        uint64_t n = total - feito; if (n > sizeof g_buf) n = sizeof g_buf;
        if (!ler_k(g_base + feito, g_buf, n)) {
            /* pedaco com buraco: tenta de 64 KB em 64 KB, o que falhar vira zero */
            for (uint64_t k = 0; k < n; k += 0x10000) {
                uint64_t m = n - k < 0x10000 ? n - k : 0x10000;
                if (!ler_k(g_base + feito + k, g_buf + k, m)) { memset(g_buf + k, 0, m); falhas++; }
            }
        }
        int w = sceKernelWrite(fd, g_buf, n);
        if (w <= 0) { logf_("ERRO gravar em 0x%llx", (unsigned long long)feito); break; }
        feito += (uint64_t)w;
    }
    sceKernelClose(fd);
    logf_("EBOOT_GRAVADO %s bytes=%llu blocos_vazios=%llu", nome, (unsigned long long)feito, (unsigned long long)falhas);
    if (feito == total) { int m = sceKernelOpen(marca, 0x0001 | 0x0200 | 0x0400, 0666); if (m >= 0) sceKernelClose(m); }
}
static void assinaturas(void)
{
    hexlog("CHAVEAMENTO", 0xd69053ULL, 16);
    hexlog("OITAVAS", 0xd6908bULL, 16);
    hexlog("QUEDA_ELIM", 0xd694c0ULL, 16);
    hexlog("LISTA_LIGA", 0xfee0f0ULL, 24);
    hexlog("CARREIRA_PTR", RVA_CARREIRA_PTR, 8);
    hexlog("TAB_COPAS", RVA_TAB_COPAS, 16);
}

static void *trabalho(void *arg)
{
    (void)arg;
    sceKernelMkdir("/data/jk", 0777);
    sceKernelMkdir(PASTA, 0777);
    struct proc_info pi; memset(&pi, 0, sizeof pi);
    if (sys_sdk_proc_info(&pi) != 0) { logf_("ERRO proc_info"); return 0; }
    g_base = pi.base_address;
    logf_("JK_SONDA v2.00 titulo=%s versao=%s base=0x%llx", pi.titleid, pi.version, (unsigned long long)g_base);
    sceKernelUsleep(15000000);   /* deixa o jogo abrir o menu antes de copiar */
    assinaturas();
    copiar_executavel(pi.titleid);
    logf_("PRONTO: pode mandar o sonda.log e o eboot_%.9s.bin (o ml_N.bin sai quando abrir a ML)", pi.titleid);

    int num = 0, auto_feito = 0, estavel = 0;
    uint64_t ultimo = 0;
    for (num = 0; num < 10; num++) { /* nao sobrescrever copias antigas */
        char nm[64]; snprintf(nm, sizeof nm, PASTA "/ml_%d.bin", num);
        if (!existe(nm)) break;
    }
    while (g_rodando) {
        sceKernelUsleep(2000000);
        uint64_t db = carreira();
        if (!db) { estavel = 0; ultimo = 0; auto_feito = 0; continue; }
        if (db != ultimo) { ultimo = db; estavel = 0; logf_("ML_DETECTADA db=0x%llx", (unsigned long long)db); }
        if (++estavel < 5) continue; /* espera ~10s com a ML parada */
        int pedido = existe(PASTA "/copiar.txt");
        if ((!auto_feito || pedido) && num < 10) {
            resumo(db, num);
            copiar(db, num);
            num++;
            auto_feito = 1;
            if (pedido) sceKernelUnlink(PASTA "/copiar.txt");
        }
    }
    return 0;
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    scePthreadCreate(&g_thr, 0, trabalho, 0, "jk_sonda");
    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    g_rodando = 0;
    return 0;
}

int32_t attr_module_hidden module_start(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
int32_t attr_module_hidden module_stop(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
