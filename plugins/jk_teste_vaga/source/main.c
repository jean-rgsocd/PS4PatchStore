/*
 * JK TESTE VAGA - plugin de teste para PES 2021 (CUSA18676).
 * Escreve um grupo de times de teste numa competicao que fica vazia a temporada
 * inteira (confirmado via jk_sonda: tipo=40 fase=0), so pra ver como o jogo
 * desenha essa competicao no menu. Nao mexe em nenhum outro slot.
 *
 * Gatilhos (crie o arquivo vazio por FTP em /data/jk/super/):
 *   testar.txt    -> escreve os 4 times de teste no slot alvo
 *   reverter.txt  -> desfaz o teste (volta n=0 nesse slot)
 * O plugin apaga o arquivo de gatilho depois de processar.
 * Log em /data/jk/super/teste_vaga.log
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

attr_public const char *g_pluginName = "jk_teste_vaga";
attr_public const char *g_pluginDesc = "JK PATCHS - teste de escrita em competicao vazia";
attr_public const char *g_pluginAuth = "JK PATCHS";
attr_public uint32_t g_pluginVersion = 0x00000100;

int jk_qmp(void *addr, void **start, void **end, int *prot) __asm__("sceKernelQueryMemoryProtection");

/* Enderecos (eboot 01.08 Mega/Brasfoot, mesmos do jk_sonda v2.00) */
#define RVA_CARREIRA_PTR   0x5b4e398ULL
#define DB_TAMANHO         0x1db7000ULL
#define DB_COMP_TAB        0x1163cc4ULL   /* competicoes: 0x388 bytes cada */
#define DB_COMP_N          0x1273364ULL   /* u32 quantidade (<=300) */
#define DB_COMP_STRIDE     0x388

#define PASTA "/data/jk/super"

/* alvo do teste: tipo 40, fase 0 (slot confirmado vazio dez->fev->mar) */
#define TIPO_ALVO  40
#define FASE_ALVO  0
#define KEY_ALVO   ((uint16_t)(TIPO_ALVO | (FASE_ALVO << 10)))

/* times de teste (IDs ja usados em outras competicoes, so pra ver o encaixe visual) */
static const uint32_t TIMES_TESTE[] = { 101, 107, 172, 108 };
#define N_TIMES_TESTE ((uint32_t)(sizeof(TIMES_TESTE) / sizeof(TIMES_TESTE[0])))

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

/* igual legivel(), mas exige tambem permissao de escrita */
static int escrevivel(uint64_t a, uint64_t n)
{
    if (a < 0x10000 || n > ~a) return 0;
    uint64_t fim = a + n;
    while (a < fim) {
        void *s = 0, *e = 0; int p = 0;
        if (jk_qmp((void *)a, &s, &e, &p) != 0) return 0;
        if ((p & 3) != 3) return 0; /* precisa ler E escrever */
        if ((uint64_t)e <= a) return 0;
        a = (uint64_t)e;
    }
    return 1;
}

static uint64_t ler_ptr(uint64_t a) { return legivel(a, 8) ? *(volatile uint64_t *)a : 0; }

static int g_log = -1;
static void logf_(const char *fmt, ...)
{
    char b[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    if (g_log < 0) g_log = sceKernelOpen(PASTA "/teste_vaga.log", 0x0001 | 0x0200 | 0x0008, 0666); /* WRONLY|CREAT|APPEND */
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

/* procura a entrada com a key pedida (tipo|fase<<10); devolve ponteiro ou 0 se nao achar */
static uint8_t *achar_slot(uint64_t db)
{
    uint8_t *d = (uint8_t *)db;
    uint32_t n = *(uint32_t *)(d + DB_COMP_N);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *c = d + DB_COMP_TAB + (uint64_t)i * DB_COMP_STRIDE;
        uint16_t key = *(uint16_t *)c;
        if (key == KEY_ALVO) return c;
    }
    return 0;
}

static void escrever_teste(uint64_t db)
{
    uint8_t *c = achar_slot(db);
    if (!c) { logf_("ERRO: slot key=0x%x (tipo=%d fase=%d) nao encontrado", KEY_ALVO, TIPO_ALVO, FASE_ALVO); return; }
    if (!escrevivel((uint64_t)c, DB_COMP_STRIDE)) { logf_("ERRO: slot nao e gravavel"); return; }
    uint32_t atual = c[0x37e] & 0x7f;
    logf_("ANTES: slot encontrado key=0x%x, n atual=%u", KEY_ALVO, atual);
    for (uint32_t k = 0; k < N_TIMES_TESTE; k++) {
        uint32_t v = (TIMES_TESTE[k] & 0x1ffff) << 14;
        *(volatile uint32_t *)(c + 0x1e4 + k * 4) = v;
    }
    uint8_t byte_atual = c[0x37e];
    c[0x37e] = (uint8_t)((byte_atual & 0x80) | (N_TIMES_TESTE & 0x7f));
    logf_("ESCRITO: key=0x%x n=%u times=%u,%u,%u,%u", KEY_ALVO, N_TIMES_TESTE,
          TIMES_TESTE[0], TIMES_TESTE[1], TIMES_TESTE[2], TIMES_TESTE[3]);
}

static void reverter_teste(uint64_t db)
{
    uint8_t *c = achar_slot(db);
    if (!c) { logf_("ERRO reverter: slot nao encontrado"); return; }
    if (!escrevivel((uint64_t)c, DB_COMP_STRIDE)) { logf_("ERRO reverter: slot nao e gravavel"); return; }
    uint8_t byte_atual = c[0x37e];
    c[0x37e] = (uint8_t)(byte_atual & 0x80);
    for (uint32_t k = 0; k < N_TIMES_TESTE; k++)
        *(volatile uint32_t *)(c + 0x1e4 + k * 4) = 0;
    logf_("REVERTIDO: key=0x%x voltou para n=0", KEY_ALVO);
}

static void *trabalho(void *arg)
{
    (void)arg;
    sceKernelMkdir("/data/jk", 0777);
    sceKernelMkdir(PASTA, 0777);
    struct proc_info pi; memset(&pi, 0, sizeof pi);
    if (sys_sdk_proc_info(&pi) != 0) { logf_("ERRO proc_info"); return 0; }
    g_base = pi.base_address;
    logf_("JK_TESTE_VAGA titulo=%s base=0x%llx alvo_key=0x%x (tipo=%d fase=%d)",
          pi.titleid, (unsigned long long)g_base, KEY_ALVO, TIPO_ALVO, FASE_ALVO);

    while (g_rodando) {
        sceKernelUsleep(2000000);
        uint64_t db = carreira();
        if (!db) continue;
        if (existe(PASTA "/testar.txt")) {
            escrever_teste(db);
            sceKernelUnlink(PASTA "/testar.txt");
        }
        if (existe(PASTA "/reverter.txt")) {
            reverter_teste(db);
            sceKernelUnlink(PASTA "/reverter.txt");
        }
    }
    return 0;
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    scePthreadCreate(&g_thr, 0, trabalho, 0, "jk_teste_vaga");
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
