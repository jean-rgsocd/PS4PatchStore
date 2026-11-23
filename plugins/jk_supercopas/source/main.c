/*
 * JK SUPERCOPAS v1.00 - PES 2021 (CUSA18676, eboot 01.08 Mega/Brasfoot)
 *
 * Aproveita duas supercopas nativas da Master League (jogo unico, penaltis e taca do proprio jogo):
 *   Supercopa Uruguaya (chave 0xa5, 1 de fevereiro)   -> "Supercopa Rei"   (campeao brasileiro x campeao da Copa do Brasil)
 *   Super Taca Colombiana (chave 0x83, 17 de janeiro) -> "CONMEBOL Recopa" (campeao da Libertadores x campeao da Sul-Americana)
 *
 * Troca os 2 clubes no jogo marcado (antes da data), no confronto, na lista de inscritos e no nome.
 * Se o clube do usuario entrar, grava o jogo na agenda dele.
 *
 * Campeoes: /data/jk/super/campeoes-ANO.txt (ANO = temporada anterior). Sem arquivo, 2025 usa os campeoes reais:
 *   BRASILEIRO=1248 (Flamengo)  VICE_BRASILEIRO=137 (Palmeiras)  COPA_DO_BRASIL=1247 (Corinthians)
 *   LIBERTADORES=1248 (Flamengo)  SULAMERICANA=1929 (Lanus)
 * Log: /data/jk/super/supercopas.log
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

attr_public const char *g_pluginName = "jk_supercopas";
attr_public const char *g_pluginDesc = "JK PATCHS - Supercopa Rei e Recopa na Master League";
attr_public const char *g_pluginAuth = "JK PATCHS";
attr_public uint32_t g_pluginVersion = 0x00000100;

int jk_qmp(void *addr, void **start, void **end, int *prot) __asm__("sceKernelQueryMemoryProtection");

#define RVA_CARREIRA_PTR 0x5b4e398ULL
#define DB_TAMANHO       0x1db7000ULL
#define DB_COMP_TAB      0x1163cc4ULL
#define DB_COMP_N        0x1273364ULL
#define COMP_STRIDE      0x388
#define COMP_INSCRITOS   0x1e4
#define COMP_N_INSCR     0x37e
#define COMP_NOME1       0x02
#define COMP_NOME2       0x75
#define COMP_NOME_TAM    0x73
#define DB_CONFRONTOS    0x12cd5d4ULL
#define DB_JOGOS         0x1407580ULL
#define JOGO_STRIDE      0x254
#define JOGOS_MAX        17041
#define DB_DATA          0x1baa094ULL   /* u16 dia do ano (0..364), u16 ano */
#define DB_AGENDA        0x1baa0a8ULL   /* 365 x 16 bytes */
#define DB_CLUBE_USUARIO 0x1cef14cULL

#define PASTA "/data/jk/super"

static uint64_t g_base;
static volatile int g_rodando = 1;
static OrbisPthread g_thr;

/* ---------- utilitarios ---------- */
static int legivel(uint64_t a, uint64_t n)
{
    if (a < 0x10000 || n > ~a) return 0;
    uint64_t fim = a + n;
    while (a < fim) {
        void *s = 0, *e = 0; int p = 0;
        if (jk_qmp((void *)a, &s, &e, &p) != 0) return 0;
        if ((p & 3) != 3) return 0;              /* leitura + escrita */
        if ((uint64_t)e <= a) return 0;
        a = (uint64_t)e;
    }
    return 1;
}
static uint64_t ler_ptr(uint64_t a)
{
    void *s = 0, *e = 0; int p = 0;
    if (a < 0x10000 || jk_qmp((void *)a, &s, &e, &p) != 0 || !(p & 1)) return 0;
    return *(volatile uint64_t *)a;
}
#define U16(p) (*(volatile uint16_t *)(p))
#define U32(p) (*(volatile uint32_t *)(p))

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
    if (g_log < 0) g_log = sceKernelOpen(PASTA "/supercopas.log", 0x0001 | 0x0200 | 0x0008, 0666);
    if (g_log >= 0) sceKernelWrite(g_log, b, n);
}

static uint64_t carreira(void)
{
    uint64_t obj = ler_ptr(g_base + RVA_CARREIRA_PTR);
    if (!obj) return 0;
    uint64_t db = ler_ptr(obj + 0x58);
    if (!db || !legivel(db, DB_TAMANHO)) return 0;
    uint32_t n = U32(db + DB_COMP_N);
    if (n == 0 || n > 300) return 0;
    return db;
}

static const uint16_t g_mes_ini[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
static int dia_do_ano(int mes, int dia) { if (mes < 1 || mes > 12) return -1; return g_mes_ini[mes - 1] + dia - 1; }

/* ---------- campeoes ---------- */
typedef struct { int br, vice_br, cdb, lib, sula; } campeoes_t;

static int ler_campeoes(int ano, campeoes_t *c)
{
    memset(c, 0, sizeof *c);
    char nome[96]; snprintf(nome, sizeof nome, PASTA "/campeoes-%d.txt", ano);
    int fd = sceKernelOpen(nome, 0, 0);
    if (fd >= 0) {
        char t[1024]; int n = sceKernelRead(fd, t, sizeof t - 1); sceKernelClose(fd);
        if (n < 0) n = 0;
        t[n] = 0;
        char *p = t;
        while (*p) {
            char *fim = p; while (*fim && *fim != '\n' && *fim != '\r') fim++;
            char sv = *fim; *fim = 0;
            char *ig = strchr(p, '=');
            if (ig) {
                int v = 0; for (char *q = ig + 1; *q >= '0' && *q <= '9'; q++) v = v * 10 + (*q - '0');
                *ig = 0;
                if (!strcmp(p, "BRASILEIRO")) c->br = v;
                else if (!strcmp(p, "VICE_BRASILEIRO")) c->vice_br = v;
                else if (!strcmp(p, "COPA_DO_BRASIL")) c->cdb = v;
                else if (!strcmp(p, "LIBERTADORES")) c->lib = v;
                else if (!strcmp(p, "SULAMERICANA")) c->sula = v;
            }
            *fim = sv; p = *fim ? fim + 1 : fim;
        }
        logf_("CAMPEOES ano=%d arquivo=%s br=%d vice=%d cdb=%d lib=%d sula=%d", ano, nome, c->br, c->vice_br, c->cdb, c->lib, c->sula);
        return 1;
    }
    if (ano == 2025) {
        c->br = 1248; c->vice_br = 137; c->cdb = 1247; c->lib = 1248; c->sula = 1929;
        logf_("CAMPEOES ano=2025 padrao (reais) br=1248 vice=137 cdb=1247 lib=1248 sula=1929");
        return 1;
    }
    return 0;
}

/* id do time -> valor completo (id<<14 | indice), achado nas listas de inscritos */
static uint32_t valor_time(uint64_t db, int id)
{
    uint32_t n = U32(db + DB_COMP_N);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t c = db + DB_COMP_TAB + (uint64_t)i * COMP_STRIDE;
        uint32_t q = *(volatile uint8_t *)(c + COMP_N_INSCR) & 0x7f; if (q > 48) q = 48;
        for (uint32_t k = 0; k < q; k++) {
            uint32_t v = U32(c + COMP_INSCRITOS + k * 4);
            if ((int)((v >> 14) & 0x1ffff) == id) return v;
        }
    }
    return 0;
}

static void escrever_nome(uint64_t campo, const char *nome)
{
    volatile uint8_t *p = (volatile uint8_t *)campo;
    size_t n = strlen(nome); if (n > COMP_NOME_TAM - 1) n = COMP_NOME_TAM - 1;
    for (size_t i = 0; i < COMP_NOME_TAM; i++) p[i] = i < n ? (uint8_t)nome[i] : 0;
}

/* ---------- uma supercopa ---------- */
typedef struct {
    uint16_t chave;      /* competicao nativa reaproveitada */
    const char *nome;
    const char *rotulo;
    int feito_ano;       /* ultimo ano ja aplicado/verificado */
} copa_t;

static copa_t g_copas[2] = {
    { 0xa5, "Supercopa Rei",   "SUPERCOPA", 0 },
    { 0x83, "CONMEBOL Recopa", "RECOPA",    0 },
};

static void renomear(uint64_t db, uint16_t chave, const char *nome)
{
    uint32_t n = U32(db + DB_COMP_N);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t c = db + DB_COMP_TAB + (uint64_t)i * COMP_STRIDE;
        if ((U16(c) & 0x3ff) != chave) continue;
        if (strncmp((const char *)(c + COMP_NOME1), nome, strlen(nome) + 1) != 0) {
            escrever_nome(c + COMP_NOME1, nome);
            escrever_nome(c + COMP_NOME2, nome);
        }
    }
}

static int aplicar(uint64_t db, copa_t *cp, int ano, int hoje, int ta, int tb)
{
    uint32_t va = valor_time(db, ta), vb = valor_time(db, tb);
    if (!va || !vb) {
        if (cp->feito_ano != ano) logf_("%s ERRO time_nao_achado a=%d(0x%x) b=%d(0x%x)", cp->rotulo, ta, va, tb, vb);
        cp->feito_ano = ano; return -1;
    }

    /* 1) jogo marcado desta temporada */
    int idx = -1, doy = -1;
    for (int i = 0; i < JOGOS_MAX; i++) {
        uint64_t j = db + DB_JOGOS + (uint64_t)i * JOGO_STRIDE;
        if (U16(j + 2) != cp->chave) continue;
        if (U16(j + 8) != ano) continue;
        idx = i; doy = dia_do_ano(*(volatile uint8_t *)(j + 10), *(volatile uint8_t *)(j + 11));
        break;
    }
    if (idx < 0) return 0;                       /* calendario ainda nao gerado */
    uint64_t j = db + DB_JOGOS + (uint64_t)idx * JOGO_STRIDE;
    uint32_t oa = U32(j + 0x14), ob = U32(j + 0x18);
    if (oa == va && ob == vb) return 1;          /* ja aplicado */
    if (doy <= hoje) {
        if (cp->feito_ano != ano) logf_("%s jogo=%d dia=%d ja_passou hoje=%d (sem troca)", cp->rotulo, idx, doy, hoje);
        cp->feito_ano = ano; return 1;
    }

    U32(j + 0x14) = va; U32(j + 0x18) = vb;

    /* 2) confronto (tabela de pares): chave, mandante, visitante, u16 indice do jogo */
    int pares = 0;
    for (uint64_t o = DB_CONFRONTOS; o + 0x10 < DB_JOGOS; o += 4) {
        if (U32(db + o) == cp->chave && U16(db + o + 0xc) == (uint16_t)idx && U32(db + o + 4) == oa && U32(db + o + 8) == ob) {
            U32(db + o + 4) = va; U32(db + o + 8) = vb; pares++;
        }
    }

    /* 3) inscritos da competicao */
    uint32_t n = U32(db + DB_COMP_N);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t c = db + DB_COMP_TAB + (uint64_t)i * COMP_STRIDE;
        if (U16(c) != cp->chave) continue;
        if ((*(volatile uint8_t *)(c + COMP_N_INSCR) & 0x7f) >= 2) {
            U32(c + COMP_INSCRITOS) = va; U32(c + COMP_INSCRITOS + 4) = vb;
        }
    }

    /* 4) agenda do usuario */
    uint32_t usu = U32(db + DB_CLUBE_USUARIO);
    const char *ag = "fora";
    if (doy >= 0 && doy < 365) {
        uint64_t e = db + DB_AGENDA + (uint64_t)doy * 16;
        int usu_novo = (usu == va || usu == vb);
        int usu_antigo = (usu == oa || usu == ob);
        if (usu_novo) {
            if (U32(e) == 0xffffffff) {
                U16(e) = (uint16_t)idx; U16(e + 2) = cp->chave;
                U32(e + 4) = U32(j + 0xc); U32(e + 8) = 2; U32(e + 12) = usu;
                ag = "gravada";
            } else if (U16(e) == (uint16_t)idx && U16(e + 2) == cp->chave) ag = "ja_estava";
            else { ag = "CONFLITO"; logf_("%s AGENDA_CONFLITO dia=%d ocupado por jogo=%u chave=0x%x", cp->rotulo, doy, U16(e), U16(e + 2)); }
        } else if (usu_antigo && U16(e) == (uint16_t)idx && U16(e + 2) == cp->chave) {
            U32(e) = 0xffffffff; U32(e + 4) = 0x37; U32(e + 8) = 3; U32(e + 12) = 0xffffffff;
            ag = "removida";
        }
    }
    logf_("%s ano=%d jogo=%d dia=%d %d-x-%d (antes %u-x-%u) confrontos=%d agenda=%s",
          cp->rotulo, ano, idx, doy, ta, tb, (oa >> 14) & 0x1ffff, (ob >> 14) & 0x1ffff, pares, ag);
    logf_("LEMBRETE salve_a_ML_agora: a troca so fica no save gravado depois desta linha");
    return 1;
}

static campeoes_t g_camp;
static int g_camp_ano = 0, g_camp_ok = 0;

static void ciclo(uint64_t db)
{
    int hoje = U16(db + DB_DATA);
    int ano = U16(db + DB_DATA + 2);
    if (ano < 2000 || ano > 2100 || hoje > 365) return;   /* carreira ainda nao comecou */

    for (int k = 0; k < 2; k++) renomear(db, g_copas[k].chave, g_copas[k].nome);

    if (g_camp_ano != ano) {                               /* le o arquivo 1x por temporada */
        g_camp_ano = ano;
        g_camp_ok = ler_campeoes(ano - 1, &g_camp);
        if (!g_camp_ok) logf_("AGUARDANDO campeoes-%d.txt (sem campeoes da temporada anterior)", ano - 1);
        g_copas[0].feito_ano = g_copas[1].feito_ano = 0;
    }
    if (!g_camp_ok) return;
    campeoes_t *c = &g_camp;
    int b_sup = (c->cdb == c->br) ? c->vice_br : c->cdb;
    if (c->br && b_sup && c->br != b_sup) aplicar(db, &g_copas[0], ano, hoje, c->br, b_sup);
    if (c->lib && c->sula && c->lib != c->sula) aplicar(db, &g_copas[1], ano, hoje, c->lib, c->sula);
}

static void *trabalho(void *arg)
{
    (void)arg;
    sceKernelMkdir("/data/jk", 0777);
    sceKernelMkdir(PASTA, 0777);
    struct proc_info pi; memset(&pi, 0, sizeof pi);
    if (sys_sdk_proc_info(&pi) != 0) { logf_("ERRO proc_info"); return 0; }
    g_base = pi.base_address;
    logf_("JK_SUPERCOPAS v1.00 titulo=%s versao=%s", pi.titleid, pi.version);
    if (strncmp(pi.titleid, "CUSA18676", 9) != 0) { logf_("titulo diferente: parado"); return 0; }

    uint64_t ultimo = 0;
    while (g_rodando) {
        sceKernelUsleep(2000000);
        uint64_t db = carreira();
        if (!db) { if (ultimo) { g_copas[0].feito_ano = g_copas[1].feito_ano = 0; g_camp_ano = 0; ultimo = 0; } continue; }
        ultimo = db;
        ciclo(db);
    }
    return 0;
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    scePthreadCreate(&g_thr, 0, trabalho, 0, "jk_supercopas");
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
