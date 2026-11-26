/*
 * JK SUPERCOPAS v1.03 - PES 2021 (CUSA18676, eboot 01.08 Mega/Brasfoot)
 *
 * Aproveita duas supercopas nativas da Master League (jogo unico, penaltis e taca do proprio jogo):
 *   Supercopa Uruguaya (chave 0xa5, 1 de fevereiro)   -> "Supercopa Rei"   (campeao brasileiro x campeao da Copa do Brasil)
 *   Super Taca Colombiana (chave 0x83, 17 de janeiro) -> "CONMEBOL Recopa" (campeao da Libertadores x campeao da Sul-Americana)
 *
 * Troca os 2 clubes no jogo marcado (antes da data), no confronto, na lista de inscritos e no nome.
 * Se o clube do usuario entrar, grava o jogo na agenda dele.
 *
 * v1.16: roda tambem no Demisan (CUSA22062).
 * v1.15: aba Eliminatorias: liga o bit 0x100 (+0x378) do mata-mata da Sula, que o jogo le no menu.
 * v1.14: aba Eliminatorias apagada no mata-mata: o "a definir" nao aponta mais para rodada ja sorteada pelo RB.
 * v1.13: a aba (categoria) so muda depois que o jogo da copa existe no calendario: numa ML nova a Recopa
 *        nos internacionais impedia o jogo de criar a partida dela.
 * v1.12: Eliminatorias da Sula antes do mata-mata (ex.: marco, entre a 1a fase e os grupos) caiam:
 *        a correcao do chaveamento agora troca de modo conforme a fase (byte 0xd69054).
 * v1.11: quando a competicao nao tem jogo na temporada, grava SEM_JOGO e lista (DIAG) os jogos dela.
 * v1.10: campeao da Sula: lia a competicao errada (0x0c = grupos; o mata-mata e 0x0d) e, se o arquivo
 *        da temporada ficou com SULAMERICANA=0, completa com o campeoes.txt do plugin do RB.
 * v1.09: CORRIGE a Sula parando no playoff: o RB le a contagem de confrontos de cada rodada para
 *        saber o que sortear; os "a definir" faziam ele achar que tudo estava sorteado. Agora a rodada
 *        seguinte a uma fase terminada fica vazia para o RB sortear.
 * v1.08: chaveamento da Sula com 5 colunas (playoff, oitavas, quartas, semi, final): eboot 0xd6908b.
 *        Desligar: sem_oitavas.txt.
 * v1.07: Eliminatorias da Sula ABREM (testado no console): correcao do chaveamento (v1.06) +
 *        chaves "a definir" ligadas por padrao. Desligar as chaves: sem_chaves.txt.
 * v1.06: corrige a tela de chaveamento no proprio jogo (eboot 0xd69053, 5 bytes, conferidos antes)
 *        e desfaz o ajuste de tipo da v1.05. Desligar: sem_chaveamento.txt.
 * v1.05: CAUSA da queda nas Eliminatorias da Sula (queda.log, eboot 0xd694c0): a primeira fase (0xe)
 *        era tipo 3 e vinha antes do mata-mata na tabela. Ela vira tipo 2 enquanto o mata-mata tem
 *        confrontos. Chaves "a definir" desligadas (com_chaves.txt liga). Desligar tudo: sem_fase.txt.
 * v1.04d: causa achada pelo queda.log (eboot 0xd694c0): a Sula tinha 2 fases "mata-mata principal";
 *         a primeira fase (0xe) vira preliminar. A ligacao +0xec volta a ser a do RB (nao era a causa).
 * v1.04c: Sula - o mata-mata deixa de apontar para a fase de grupos da Libertadores (+0xec);
 *         chaves "a definir" ligadas de novo. Desligar: sem_chaves.txt.
 * v1.04b: grava /data/jk/super/queda.log quando o jogo cai (endereco, registradores, pilha).
 * v1.04: Sula - as rodadas vazias do mata-mata (oitavas, quartas, semi, final) recebem confrontos
 *        "a definir" como nas copas do jogo; sem isso a tela Eliminatorias caia (CE-34878-0).
 *        Desligar: /data/jk/super/sem_chaves.txt. O gancho da lista fica desligado (so com com_gancho.txt).
 * v1.03f: confere os bytes do jogo lendo direto (a v1.03e recusava o gancho com 00 00 00 00).
 * v1.03f: /data/jk/super/sem_categoria.txt devolve as duas copas a categoria original (teste).
 * v1.03e: o primeiro codigo do .prx volta a ser o plugin_load (o console executa o endereco 0 ao carregar;
 *        na v1.03b/c/d ali estava o trecho em assembly do gancho e o jogo caia ao abrir).
 * v1.03d: plugin_load igual ao da v1.02 (so cria a thread); v1.03/b/c travavam ao abrir o jogo por trabalhar no plugin_load.
 * v1.03c: gancho instalado so com a ML aberta (nada na abertura do jogo) e log de cada passo.
 * v1.03b: gancho gravado pelo kernel do GoldHEN (proc_rw), como o do RB; a v1.03 travava ao abrir o jogo.
 *        Para desligar o gancho: criar /data/jk/super/sem_gancho.txt
 * v1.03: categoria no menu "Categoria de competicao": Supercopa Rei na aba BRASIL e Recopa nos INTERNACIONAIS
 *        (junto da Sul-Americana). A lista de 6 copas de cada liga (onde o plugin do RB encaixa a Sula)
 *        nao recebe as nossas copas, para a Sula nunca perder o lugar. Em dezembro (virada da temporada)
 *        as duas voltam para a categoria original por seguranca e retornam em janeiro.
 * v1.02: Libertadores pelo mata-mata (chave 0x0a) e regra do gol fora no empate do agregado.
 * v1.01: os campeoes de cada temporada sao descobertos SOZINHOS na ML e gravados em campeoes-ANO.txt
 *        (Brasileirao: tabela final; Copa do Brasil e Libertadores: final com agregado/prorrogacao/penaltis;
 *         Sul-Americana: /data/PES21/sulamericana/campeoes.txt do plugin do RB, ou a final dela).
 *        Sem campeoes, as duas copas voltam ao nome original (nunca "Supercopa Rei" com outros clubes).
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
#include "jk_lic.h"
#include "lic_pub.h"

#define attr_public __attribute__((visibility("default")))
#define attr_module_hidden __attribute__((weak)) __attribute__((visibility("hidden")))

attr_public const char *g_pluginName = "jk_supercopas";
attr_public const char *g_pluginDesc = "JK PATCHS - Supercopa Rei e Recopa na Master League";
attr_public const char *g_pluginAuth = "JK PATCHS";
attr_public uint32_t g_pluginVersion = 0x00000118;

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
#define COMP_CATEG       0x380          /* u16: bits 7..12 = categoria do menu (1 internacional, 16 Brasil, 19 Colombia, 28 Uruguai) */
#define CATEG(v)         (((v) >> 7) & 0x3f)
#define RVA_LISTA_LIGA   0xfee0f0ULL    /* monta a lista (6 copas) de uma liga; o plugin do RB chama esta funcao */
#define DIA_VOLTA_CATEG  334            /* a partir de 1 de dezembro: categoria original ate virar o ano */
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

static int existe(const char *p) { OrbisKernelStat st; return sceKernelStat(p, &st) == 0; }

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
    const char *nome_original;
    const char *rotulo;
    int feito_ano;       /* ultimo ano ja aplicado/verificado */
    int regra;           /* id da regra (lista de copas) */
    int cat_nova, cat_original;
    int diag_ano;        /* ultimo ano em que o diagnostico 'sem jogo' foi gravado (fica no fim: os inicializadores nao o citam) */
} copa_t;

static copa_t g_copas[2] = {
    { 0xa5, "Supercopa Rei",   "Supercopa Uruguaya",            "SUPERCOPA", 0, 141, 16, 28 },
    { 0x83, "CONMEBOL Recopa", "Super Ta\xc3\xa7" "a Colombiana", "RECOPA",    0, 124,  1, 19 },
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

/* categoria do menu "Categoria de competicao" (so mexe se estiver na original ou na nova) */
static void categoria(uint64_t db, copa_t *cp, int nova)
{
    static int avisado[2][64];
    int qual = (cp == &g_copas[0]) ? 0 : 1;
    int alvo = nova ? cp->cat_nova : cp->cat_original;
    uint32_t n = U32(db + DB_COMP_N);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t c = db + DB_COMP_TAB + (uint64_t)i * COMP_STRIDE;
        if ((U16(c) & 0x3ff) != cp->chave) continue;
        uint16_t v = U16(c + COMP_CATEG);
        int atual = CATEG(v);
        if (atual == alvo) continue;
        if (atual != cp->cat_nova && atual != cp->cat_original) {
            if (!avisado[qual][atual]) { avisado[qual][atual] = 1; logf_("CATEGORIA %s chave=0x%x categoria=%d desconhecida: nao mexi", cp->rotulo, U16(c), atual); }
            continue;
        }
        U16(c + COMP_CATEG) = (uint16_t)((v & ~(0x3f << 7)) | (alvo << 7));
        logf_("CATEGORIA %s chave=0x%x %d -> %d (%s)", cp->rotulo, U16(c), atual, alvo,
              alvo == 16 ? "aba BRASIL" : alvo == 1 ? "INTERNACIONAIS" : "original");
    }
}

/* ---------- lista de copas de cada liga: tira as nossas para nao ocupar a vaga da Sula ----------
 * Gancho no inicio da funcao do jogo (eboot+0xfee0f0), escrito pelo kernel do GoldHEN (sys_sdk_proc_rw),
 * do mesmo jeito que o plugin do RB grava os ganchos dele. Sem mprotect e sem memoria executavel nova:
 * o comeco original da funcao (17 bytes) fica copiado aqui no proprio plugin (jk_lista_orig). */
__attribute__((visibility("hidden"))) uint64_t g_lista_volta;   /* eboot+0xfee101 */
static void *jk_lista_orig(void *saida, uint32_t liga);
/* jk_lista_orig fica no FIM do arquivo: o console executa o primeiro codigo do .prx ao carregar,
 * e ele tem que ser o plugin_load (como na v1.02). */


static int g_lista_ok = 0;
static volatile uint32_t g_lista_tiradas = 0;

static void *lista_liga_hook(void *saida, uint32_t liga)
{
    static volatile int primeira = 1;
    if (primeira) { primeira = 0; logf_("GANCHO_LISTA passo=primeira_chamada liga=%u", liga); }
    void *r = jk_lista_orig(saida, liga);
    if (saida && liga != (uint32_t)g_copas[0].regra && liga != (uint32_t)g_copas[1].regra) {
        int32_t *l = (int32_t *)saida + 1;          /* [0] = liga, [1..6] = copas */
        int k = 0, tirou = 0;
        for (int i = 0; i < 6; i++) {
            if (l[i] == g_copas[0].regra || l[i] == g_copas[1].regra) { tirou = 1; continue; }
            l[k++] = l[i];
        }
        while (k < 6) l[k++] = -1;
        if (tirou) g_lista_tiradas++;
    }
    return r;
}

static int gravar_eboot(uint64_t dest, const void *src, uint64_t n)
{
    struct proc_rw rw;
    memset(&rw, 0, sizeof rw);
    rw.address = dest; rw.data = (void *)src; rw.length = n; rw.write_flags = 1;
    if (sys_sdk_proc_rw(&rw) != 0) return 0;
    return memcmp((const void *)dest, src, n) == 0;
}

static void instalar_lista(void)
{
    static const uint8_t esperado[17] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x78};
    if (!existe(PASTA "/com_gancho.txt")) { logf_("GANCHO_LISTA desligado (padrao da v1.04; criar com_gancho.txt para testar)"); return; }
    uint64_t f = g_base + RVA_LISTA_LIGA;
    /* codigo do jogo: sempre mapeado e legivel (o legivel() so serve para os dados do save) */
    const volatile uint8_t *b = (const volatile uint8_t *)f;
    int iguais = 1;
    for (int i = 0; i < (int)sizeof esperado; i++) if (b[i] != esperado[i]) { iguais = 0; break; }
    if (!iguais) {
        logf_("GANCHO_LISTA recusado=bytes_diferentes (%02x %02x %02x %02x): as abas funcionam, a lista da liga fica sem filtro",
              b[0], b[1], b[2], b[3]);
        return;
    }
    logf_("GANCHO_LISTA passo=bytes_conferidos");
    g_lista_volta = f + sizeof esperado;
    uint8_t salto[17];
    uint64_t alvo = (uint64_t)(void *)lista_liga_hook;
    salto[0] = 0xff; salto[1] = 0x25; salto[2] = salto[3] = salto[4] = salto[5] = 0;   /* jmp [rip+0] */
    memcpy(salto + 6, &alvo, 8);
    salto[14] = salto[15] = salto[16] = 0x90;
    logf_("GANCHO_LISTA passo=gravando hook=%p volta=0x%llx", (void *)lista_liga_hook, (unsigned long long)g_lista_volta);
    if (!gravar_eboot(f, salto, sizeof salto)) {
        gravar_eboot(f, esperado, sizeof esperado);
        logf_("GANCHO_LISTA recusado=gravacao: as abas funcionam, a lista da liga fica sem filtro");
        return;
    }
    g_lista_ok = 1;
    logf_("GANCHO_LISTA ok rva=0x%llx", (unsigned long long)RVA_LISTA_LIGA);
}

static void desinstalar_lista(void)
{
    static const uint8_t esperado[17] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x78};
    if (g_lista_ok) { gravar_eboot(g_base + RVA_LISTA_LIGA, esperado, sizeof esperado); g_lista_ok = 0; }
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
    if (idx < 0) {                               /* calendario ainda nao gerado... ou o jogo nao existe nesta carreira */
        int cal = 0;                             /* so diagnostica com o calendario da temporada ja carregado */
        for (int i = 0; i < JOGOS_MAX && cal < 100; i++) { uint64_t g = db + DB_JOGOS + (uint64_t)i * JOGO_STRIDE; if (U16(g) == i && U16(g + 8) == ano) cal++; }
        if (cp->diag_ano != ano && cal >= 100) {
            cp->diag_ano = ano;
            int n = 0;
            for (int i = 0; i < JOGOS_MAX; i++) {
                uint64_t g = db + DB_JOGOS + (uint64_t)i * JOGO_STRIDE;
                if (U16(g) != i || (U16(g + 2) & 0x3ff) != (cp->chave & 0x3ff)) continue;
                if (n++ < 8)
                    logf_("%s DIAG jogo=%d comp=0x%x ano=%u mes=%u dia=%u %u-x-%u", cp->rotulo, i, U16(g + 2), U16(g + 8),
                          *(volatile uint8_t *)(g + 10), *(volatile uint8_t *)(g + 11), (U32(g + 0x14) >> 14) & 0x1ffff, (U32(g + 0x18) >> 14) & 0x1ffff);
            }
            logf_("%s SEM_JOGO ano=%d hoje=%d: nenhum jogo da competicao 0x%x nesta temporada (jogos da competicao em qualquer ano: %d)",
                  cp->rotulo, ano, hoje, cp->chave, n);
        }
        return 0;
    }
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


/* ---------- campeoes da temporada atual, achados na propria ML ---------- */
typedef struct { int idx, doy, ok, h, a, hg, ag, het, aet, ph, pa; uint16_t k; } jogo_t;

static int ler_jogo(uint64_t db, int i, jogo_t *g)
{
    uint64_t j = db + DB_JOGOS + (uint64_t)i * JOGO_STRIDE;
    volatile uint8_t *b = (volatile uint8_t *)(j + 0x1c);
    g->idx = i; g->k = U16(j + 2);
    g->doy = dia_do_ano(*(volatile uint8_t *)(j + 10), *(volatile uint8_t *)(j + 11));
    g->ok = (U16(j + 4) & 0x4000) != 0;                       /* jogo disputado */
    g->h = (U32(j + 0x14) >> 14) & 0x1ffff; g->a = (U32(j + 0x18) >> 14) & 0x1ffff;
    g->hg = b[0]; g->het = b[1]; g->ph = b[2];                 /* mesmo layout que o plugin da Sula usa */
    g->ag = b[3]; g->aet = b[4]; g->pa = b[5];
    return 1;
}

/* campeao de um mata-mata (final em jogo unico ou ida e volta). 0 = ainda nao terminou ou indefinido */
static int campeao_mata(uint64_t db, uint16_t comp, int ano, const char **metodo)
{
    int total = 0, pend = 0, ultimo = -1, nult = 0;
    jogo_t f; memset(&f, 0, sizeof f);
    for (int i = 0; i < JOGOS_MAX; i++) {
        uint64_t j = db + DB_JOGOS + (uint64_t)i * JOGO_STRIDE;
        if ((U16(j + 2) & 0x3ff) != comp || U16(j + 8) != ano) continue;
        jogo_t g; ler_jogo(db, i, &g); total++;
        if (!g.ok || g.h >= 0x1ffff || g.a >= 0x1ffff || !g.h || !g.a) { pend++; continue; }   /* sem clube definido = fase ainda por vir */
        if (g.doy > ultimo) { ultimo = g.doy; nult = 1; f = g; } else if (g.doy == ultimo) nult++;
    }
    *metodo = "nada";
    if (!total) return 0;
    if (pend) { *metodo = "em_andamento"; return 0; }
    if (nult != 1) { *metodo = "final_ambigua"; return 0; }
    /* ida: mesmo confronto invertido, mesma fase, ate 21 dias antes */
    jogo_t ida; int tem_ida = 0;
    for (int i = 0; i < JOGOS_MAX; i++) {
        uint64_t j = db + DB_JOGOS + (uint64_t)i * JOGO_STRIDE;
        if (U16(j + 2) != f.k || U16(j + 8) != ano) continue;
        jogo_t g; ler_jogo(db, i, &g);
        if (g.h == f.a && g.a == f.h && g.ok && g.doy < f.doy && f.doy - g.doy <= 21 && (!tem_ida || g.doy > ida.doy)) { ida = g; tem_ida = 1; }
    }
    if (tem_ida) {
        int tx = ida.hg + ida.het + f.ag + f.aet;              /* x = mandante da ida = visitante da volta */
        int ty = ida.ag + ida.aet + f.hg + f.het;
        if (tx != ty) { *metodo = "agregado"; return tx > ty ? f.a : f.h; }
        if (f.ph != f.pa) { *metodo = "penaltis"; return f.ph > f.pa ? f.h : f.a; }
        int fx = f.ag + f.aet, fy = ida.ag + ida.aet;           /* gols fora (o jogo usa essa regra nas eliminatorias) */
        if (fx != fy) { *metodo = "gol_fora"; return fx > fy ? f.a : f.h; }
        *metodo = "empate_sem_penaltis"; return 0;
    }
    int th = f.hg + f.het, ta = f.ag + f.aet;
    if (th != ta) { *metodo = "jogo_unico"; return th > ta ? f.h : f.a; }
    if (f.ph != f.pa) { *metodo = "penaltis"; return f.ph > f.pa ? f.h : f.a; }
    *metodo = "empate_sem_penaltis"; return 0;
}

/* campeao e vice de liga pontos corridos (chave completa, ex.: 0x1d). 0 = ainda falta jogo */
typedef struct { int id, pts, v, sg, gp; } linha_t;
static int campeao_liga(uint64_t db, uint16_t chave, int ano, int *vice)
{
    static linha_t t[64]; int n = 0, jogos = 0;
    *vice = 0;
    for (int i = 0; i < JOGOS_MAX; i++) {
        uint64_t j = db + DB_JOGOS + (uint64_t)i * JOGO_STRIDE;
        if (U16(j + 2) != chave || U16(j + 8) != ano) continue;
        jogo_t g; ler_jogo(db, i, &g); jogos++;
        if (!g.ok || g.h >= 0x1ffff || g.a >= 0x1ffff || !g.h || !g.a) return 0;
        int ids[2] = { g.h, g.a }, gf[2] = { g.hg, g.ag };
        for (int s = 0; s < 2; s++) {
            int k = 0; while (k < n && t[k].id != ids[s]) k++;
            if (k == n) { if (n >= 64) return 0; memset(&t[n], 0, sizeof t[n]); t[n].id = ids[s]; n++; }
            int a = gf[s], b = gf[1 - s];
            t[k].pts += a > b ? 3 : a == b ? 1 : 0; t[k].v += a > b; t[k].sg += a - b; t[k].gp += a;
        }
    }
    if (jogos < 10 || n < 2) return 0;
    int p1 = -1, p2 = -1;
    for (int k = 0; k < n; k++) {
        #define MELHOR(x, y) (t[x].pts != t[y].pts ? t[x].pts > t[y].pts : t[x].v != t[y].v ? t[x].v > t[y].v : t[x].sg != t[y].sg ? t[x].sg > t[y].sg : t[x].gp > t[y].gp)
        if (p1 < 0 || MELHOR(k, p1)) { p2 = p1; p1 = k; }
        else if (p2 < 0 || MELHOR(k, p2)) p2 = k;
    }
    *vice = p2 >= 0 ? t[p2].id : 0;
    return t[p1].id;
}

/* campeao da Sula gravado pelo plugin do RB: linhas "ANO=2026 CAMPEAO=1929 ..." */
static int sula_do_rb(int ano)
{
    int fd = sceKernelOpen("/data/PES21/sulamericana/campeoes.txt", 0, 0);
    if (fd < 0) return 0;
    static char t[8192]; int n = (int)sceKernelRead(fd, t, sizeof t - 1); sceKernelClose(fd);
    if (n <= 0) return 0;
    t[n] = 0;
    int achado = 0; char *p = t;
    while (*p) {
        char *e = p; while (*e && *e != '\n') e++;
        char sv = *e; *e = 0;
        char *pa = strstr(p, "ANO="), *pc = strstr(p, "CAMPEAO=");
        if (pa && pc) {
            int a = 0, c = 0;
            for (char *q = pa + 4; *q >= '0' && *q <= '9'; q++) a = a * 10 + (*q - '0');
            for (char *q = pc + 8; *q >= '0' && *q <= '9'; q++) c = c * 10 + (*q - '0');
            if (a == ano && c) achado = c > 0x1ffff ? (c >> 14) & 0x1ffff : c;
        }
        *e = sv; p = *e ? e + 1 : e;
    }
    return achado;
}

static campeoes_t g_atual; static int g_atual_ano = 0;

static void gravar_campeoes(int ano, const campeoes_t *c)
{
    char nome[96]; snprintf(nome, sizeof nome, PASTA "/campeoes-%d.txt", ano);
    char t[512];
    int n = snprintf(t, sizeof t, "# gravado sozinho pelo jk_supercopas (temporada %d)\nBRASILEIRO=%d\nVICE_BRASILEIRO=%d\nCOPA_DO_BRASIL=%d\nLIBERTADORES=%d\nSULAMERICANA=%d\n",
                     ano, c->br, c->vice_br, c->cdb, c->lib, c->sula);
    int fd = sceKernelOpen(nome, 0x0001 | 0x0200 | 0x0400, 0666);
    if (fd >= 0) { sceKernelWrite(fd, t, n); sceKernelClose(fd); }
}

/* roda a cada ciclo: quando um titulo da temporada atual fica definido, grava no arquivo */
static void acompanhar(uint64_t db, int ano, int hoje)
{
    static int ultimo_hoje = -1;
    if (g_atual_ano != ano) {                 /* comeca com o que ja estiver gravado desta temporada */
        g_atual_ano = ano; memset(&g_atual, 0, sizeof g_atual);
        campeoes_t c;
        char nome[96]; snprintf(nome, sizeof nome, PASTA "/campeoes-%d.txt", ano);
        if (existe(nome) && ler_campeoes(ano, &c)) g_atual = c;
        ultimo_hoje = -1;
    }
    if (hoje == ultimo_hoje) return;          /* so recalcula quando o dia muda */
    ultimo_hoje = hoje;
    if (hoje < 240) return;                   /* titulos so se definem no 2o semestre */
    campeoes_t c = g_atual; const char *m1 = "", *m2 = "", *m3 = "";
    int vice = 0, br = campeao_liga(db, 0x1d, ano, &vice);
    if (br) { c.br = br; c.vice_br = vice; }
    int cdb = campeao_mata(db, 0x1f, ano, &m1); if (cdb) c.cdb = cdb;
    int lib = campeao_mata(db, 0x0a, ano, &m2);   /* 0x09 = grupos; 0x0a = mata-mata e final */ if (lib) c.lib = lib;
    int sula = sula_do_rb(ano);
    if (!sula) { sula = campeao_mata(db, 0x0d, ano, &m3); }   /* 0x0d = mata-mata da Sula (0x0c sao os grupos) */
    if (sula) c.sula = sula;
    if (memcmp(&c, &g_atual, sizeof c) != 0) {
        g_atual = c;
        gravar_campeoes(ano, &c);
        logf_("CAMPEOES_DA_TEMPORADA ano=%d br=%d vice=%d cdb=%d(%s) lib=%d(%s) sula=%d%s",
              ano, c.br, c.vice_br, c.cdb, m1, c.lib, m2, c.sula, *m3 ? " (final da ML)" : " (plugin da Sula)");
    }
}


/* ---- Sula: chaves vazias das eliminatorias ----
 * O plugin do RB cria o mata-mata da Sula (regra 6) com 5 rodadas, mas so publica a
 * primeira (playoff). As outras ficam com 0 confrontos e a tela "Eliminatorias" do jogo
 * cai (CE-34878-0). Aqui as rodadas vazias recebem confrontos "a definir" no mesmo
 * formato que o jogo usa nas copas dele (times ffffffff, sem jogos, ligacoes entre fases).
 * Quando o RB publica a rodada de verdade, ele grava por cima. */
#define RODADA_STRIDE 0x208
#define SULA_REGRA    6
static int g_sem_chaves = -1, g_com_chaves = -1;
static uint32_t ref_fase(int idx, int fase, uint32_t chave) { return ((uint32_t)((idx << 6) | fase) << 16) | (chave & 0xffff); }
static int vagas_do_codigo(int cod)
{
    switch (cod) { case 0xbc: case 0xc0: return 8; case 0xcc: return 4; case 0xd0: return 2; case 0xd4: return 1; }
    return 0;
}
/* rodada so com confrontos "a definir" (times e jogos vazios): e nossa, pode esvaziar */
static int rodada_a_definir(uint64_t r)
{
    int n = *(volatile uint8_t *)(r + 0x204);
    if (n <= 0 || n > 16) return 0;
    for (int t = 0; t < n; t++) {
        uint64_t c = r + 4 + t * 0x20;
        if (U32(c) != 0xffffffff || U32(c + 4) != 0xffffffff || U32(c + 8) != 0xffffffff) return 0;
    }
    return 1;
}
/* todos os jogos da rodada ja foram disputados (byte 5 do jogo, bit 0x40) */
static int rodada_terminada(uint64_t db, uint64_t r)
{
    int n = *(volatile uint8_t *)(r + 0x204);
    if (n <= 0 || n > 16) return 0;
    for (int t = 0; t < n; t++) {
        uint64_t c = r + 4 + t * 0x20;
        if (U32(c) == 0xffffffff || U32(c + 4) == 0xffffffff) return 0;   /* ainda "a definir" */
        uint32_t jj = U32(c + 8);
        for (int l = 0; l < 2; l++) {
            uint32_t id = l ? (jj >> 16) : (jj & 0xffff);
            if (id == 0xffff) continue;
            if (id >= JOGOS_MAX) return 0;
            uint64_t g = db + DB_JOGOS + (uint64_t)id * JOGO_STRIDE;
            if (U16(g) != id || !(*(volatile uint8_t *)(g + 5) & 0x40)) return 0;
        }
        if ((jj & 0xffff) == 0xffff && (jj >> 16) == 0xffff) return 0;
    }
    return 1;
}
static void chaves_sula(uint64_t db)
{
    static int conta = 0;
    if (g_sem_chaves < 0 || ++conta >= 15) {
        conta = 0;
        int s = existe(PASTA "/sem_fase.txt");
        if (s != g_sem_chaves) logf_(s ? "FASE_SULA desligado (sem_fase.txt)" : "FASE_SULA ligado");
        int c = !existe(PASTA "/sem_chaves.txt");             /* v1.07: ligado por padrao (sem isso o chaveamento cai) */
        if (c != g_com_chaves) logf_(c ? "CHAVES_SULA ligado" : "CHAVES_SULA desligado (sem_chaves.txt)");
        g_com_chaves = c;
        g_sem_chaves = s;
    }
    if (g_sem_chaves) return;
    uint32_t n = U32(db + DB_COMP_N);
    if (n > 400) return;
    /* v1.05 marcava a primeira fase da Sula (0xe) como tipo 2; isso fez o jogo cair de outro jeito.
     * Aqui ela volta a tipo 3 (o original do RB), caso o save tenha ficado com a marca. */
    for (uint32_t i = 0; i < n; i++) {
        uint64_t cp = db + DB_COMP_TAB + (uint64_t)i * COMP_STRIDE;
        if (U32(cp + 0xf4) != SULA_REGRA || U32(cp + 0xf8) != 2 || U32(cp + 0xfc) == 0xffffffff) continue;
        U32(cp + 0xf8) = 3;
        logf_("FASE_SULA comp=%u chave=0x%x tipo 2 -> 3 (desfeito o ajuste da v1.05)", i, U16(cp));
    }
    if (!g_com_chaves) return;
    /* v1.09: o plugin do RB descobre qual fase sortear pelo numero de confrontos de cada rodada
     * (rb.prx 0x8448: contagem > 0 = fase ja publicada). Com "a definir" em todas, ele achava que
     * tudo ja estava publicado e parava no playoff. Agora:
     *   - a rodada seguinte a uma fase JA TERMINADA fica vazia (o RB ve 0 e sorteia ela);
     *   - so as rodadas depois da fase em andamento recebem "a definir" (o RB nao olha para elas
     *     enquanto a fase atual nao termina, e quando termina a gente esvazia a proxima). */
    for (uint32_t i = 0; i < n; i++) {
        uint64_t cp = db + DB_COMP_TAB + (uint64_t)i * COMP_STRIDE;
        if (U32(cp + 0xf4) != SULA_REGRA || U32(cp + 0xf8) != 3) continue;
        uint32_t rod[8]; int nr = 0;
        for (int k = 0; k < 8; k++) { uint32_t r = U32(cp + 0xfc + 4 * k); if (r == 0xffffffff) break; rod[nr++] = r; }
        if (nr < 2 || rod[0] > 0x2000) continue;
        uint64_t r0 = db + DB_CONFRONTOS + (uint64_t)rod[0] * RODADA_STRIDE;
        if (*(volatile uint8_t *)(r0 + 0x204) == 0) continue;      /* playoff ainda nao publicado */
        /* v1.15: o menu da copa (eboot 0x1b27cb7) so deixa a aba Eliminatorias clicavel se a fase atual
         * tiver o bit 0x100 em +0x378. O jogo liga esse bit sozinho na Libertadores quando o mata-mata
         * comeca; no mata-mata da Sula (criado pelo RB) ninguem liga. ML antiga: 0x0002dd08 (abria);
         * ML nova: 0x0002dc08 (aba apagada). */
        if (!(U32(cp + 0x378) & 0x100)) {
            uint32_t antes = U32(cp + 0x378);
            U32(cp + 0x378) = antes | 0x100;
            logf_("CHAVES_SULA comp=%u aba Eliminatorias liberada (+0x378 0x%08x -> 0x%08x)", i, antes, antes | 0x100);
        }
        for (int k = 1; k < nr; k++) {
            if (rod[k] > 0x2000 || rod[k - 1] > 0x2000) break;
            uint64_t r = db + DB_CONFRONTOS + (uint64_t)rod[k] * RODADA_STRIDE;
            uint64_t ra = db + DB_CONFRONTOS + (uint64_t)rod[k - 1] * RODADA_STRIDE;
            volatile uint8_t *cnt = (volatile uint8_t *)(r + 0x204);
            int cod = *(volatile uint8_t *)(r + 0x207);
            int vagas = vagas_do_codigo(cod), fase = cod >> 2;
            if (!vagas) continue;
            int anterior_terminou = rodada_terminada(db, ra);
            if (*cnt != 0 && k >= 2 && rodada_a_definir(r) && !rodada_a_definir(ra)) {
                /* v1.14: rodada anterior ja sorteada pelo RB (sem ligacao para frente). O nosso "a definir"
                 * nao pode apontar para ela (+0x18/+0x1c), senao o jogo ve o chaveamento quebrado e apaga
                 * a aba Eliminatorias. Fica igual ao das oitavas: sem ligacao para tras. */
                int mudou = 0;
                for (int t = 0; t < *cnt && t < 16; t++) {
                    uint64_t c = r + 4 + t * 0x20;
                    if (U32(c + 0x18) != 0x07f7ffff || U32(c + 0x1c) != 0x07f7ffff) { U32(c + 0x18) = 0x07f7ffff; U32(c + 0x1c) = 0x07f7ffff; mudou = 1; }
                }
                if (mudou) logf_("CHAVES_SULA comp=%u rodada=0x%x ligacao com a fase anterior (sorteada pelo RB) limpa", i, rod[k]);
            }
            if (*cnt != 0) {
                /* "a definir" nosso numa rodada que o RB precisa sortear agora: esvazia */
                if (anterior_terminou && rodada_a_definir(r)) {
                    for (int t = 0; t < 16; t++) {
                        uint64_t c = r + 4 + t * 0x20;
                        U32(c + 0x00) = 0xffffffff; U32(c + 0x04) = 0xffffffff; U32(c + 0x08) = 0xffffffff;
                        U32(c + 0x0c) = 0x07f7ffff; U32(c + 0x10) = 0x07f7ffff; U32(c + 0x14) = 0x07f7ffff;
                        U32(c + 0x18) = 0x07f7ffff; U32(c + 0x1c) = 0x07f7ffff;
                    }
                    *cnt = 0;
                    logf_("CHAVES_SULA comp=%u rodada=0x%x liberada: a fase anterior terminou, o RB sorteia esta", i, rod[k]);
                }
                continue;
            }
            if (anterior_terminou) continue;                          /* vazia de proposito: e a vez do RB */
            int livre = 1;
            for (int t = 0; t < 16; t++) if (U32(r + 4 + t * 0x20) != 0xffffffff || U32(r + 8 + t * 0x20) != 0xffffffff) livre = 0;
            if (!livre) continue;
            uint32_t chave = U32(r);
            int prox = (k + 1 < nr) ? (*(volatile uint8_t *)(db + DB_CONFRONTOS + (uint64_t)rod[k + 1] * RODADA_STRIDE + 0x207) >> 2) : -1;
            int ant = (k - 1 >= 1 && rodada_a_definir(ra)) ? (*(volatile uint8_t *)(ra + 0x207) >> 2) : -1;   /* v1.14: so liga com "a definir" nosso */
            for (int t = 0; t < vagas; t++) {
                uint64_t c = r + 4 + t * 0x20;
                U32(c + 0x00) = 0xffffffff;  U32(c + 0x04) = 0xffffffff;  U32(c + 0x08) = 0xffffffff;
                U32(c + 0x0c) = ref_fase(t, fase, chave);
                U32(c + 0x10) = (prox >= 0 && cod != 0xd4) ? ref_fase(t >> 1, prox, chave) : 0x07f7ffff;
                U32(c + 0x14) = 0x07f7ffff;
                U32(c + 0x18) = (ant >= 0) ? ref_fase(2 * t, ant, chave) : 0x07f7ffff;
                U32(c + 0x1c) = (ant >= 0) ? ref_fase(2 * t + 1, ant, chave) : 0x07f7ffff;
            }
            *cnt = (uint8_t)vagas;
            logf_("CHAVES_SULA comp=%u rodada=0x%x codigo=0x%02x vagas=%d a definir", i, rod[k], cod, vagas);
        }
    }
}

/* ---- registro de queda (v1.04b) ----
 * Quando o jogo cai (CE-34878-0), grava em /data/jk/super/queda.log o endereco da instrucao
 * que caiu, os registradores e a pilha. Nao evita a queda: so registra e devolve o erro ao jogo.
 * Desligar: /data/jk/super/sem_queda.txt */
struct jk_sigact { void *handler; int flags; uint32_t mask[4]; };
int jk_sigaction(int sig, const struct jk_sigact *novo, struct jk_sigact *velho) __asm__("sigaction");
#define JK_SA_SIGINFO 0x40
static struct jk_sigact g_velho[16];
static int g_queda_fd = -1;
static volatile int g_caindo = 0;
static int hexo(char *o, uint64_t v) { for (int i = 0; i < 16; i++) { int d = (v >> (60 - 4 * i)) & 15; o[i] = d < 10 ? '0' + d : 'a' + d - 10; } return 16; }
static int txt(char *o, const char *s) { int n = 0; while (s[n]) { o[n] = s[n]; n++; } return n; }
static void queda(int sig, void *info, void *uctx)
{
    if (!g_caindo && g_queda_fd >= 0) {
        g_caindo = 1;
        static char b[4096]; int n = 0;
        uint64_t *mc = (uint64_t *)((uint8_t *)uctx + 0x10);   /* FreeBSD amd64 mcontext */
        uint64_t rip = mc[0xa0 / 8], rsp = mc[0xb8 / 8];
        uint64_t addr = info ? *(uint64_t *)((uint8_t *)info + 0x18) : 0;
        n += txt(b + n, "QUEDA sinal="); b[n++] = '0' + sig / 10; b[n++] = '0' + sig % 10;
        n += txt(b + n, " rip="); n += hexo(b + n, rip);
        n += txt(b + n, " rip-base="); n += hexo(b + n, rip - g_base);
        n += txt(b + n, " addr="); n += hexo(b + n, addr);
        n += txt(b + n, " base="); n += hexo(b + n, g_base); b[n++] = '\n';
        static const char *nm[16] = {"rdi","rsi","rdx","rcx","r8 ","r9 ","rax","rbx","rbp","r10","r11","r12","r13","r14","r15","rsp"};
        for (int i = 0; i < 16; i++) {
            n += txt(b + n, nm[i]); b[n++] = '=';
            n += hexo(b + n, i < 15 ? mc[1 + i] : rsp); b[n++] = (i % 4 == 3) ? '\n' : ' ';
        }
        n += txt(b + n, "CONTEXTO\n");                          /* bruto: confere a posicao dos registradores no PS4 */
        for (int i = 0; i < 48; i++) { n += hexo(b + n, ((uint64_t *)uctx)[i]); b[n++] = (i % 4 == 3) ? '\n' : ' '; }
        if (info) { n += txt(b + n, "INFO\n"); for (int i = 0; i < 8; i++) { n += hexo(b + n, ((uint64_t *)info)[i]); b[n++] = (i % 4 == 3) ? '\n' : ' '; } }
        n += txt(b + n, "PILHA\n");
        sceKernelWrite(g_queda_fd, b, n); n = 0;
        if (!legivel(rsp & ~7ULL, 8)) {                        /* rsp fora do lugar esperado: tenta os valores do contexto */
            for (int i = 0; i < 48; i++) { uint64_t v = ((uint64_t *)uctx)[i]; if (v > 0x10000 && legivel(v & ~7ULL, 8) && v == (v & ~7ULL)) { if ((v >> 32) == 0x7 || (v >> 36) == 0) { rsp = v; break; } } }
        }
        uint64_t *sp = (uint64_t *)(rsp & ~7ULL);
        for (int i = 0; i < 256; i++) {
            if (!legivel((uint64_t)(sp + i), 8)) break;
            uint64_t v = sp[i];
            n += hexo(b + n, v); b[n++] = (i % 4 == 3) ? '\n' : ' ';
            if (n > 3900) { sceKernelWrite(g_queda_fd, b, n); n = 0; }
        }
        n += txt(b + n, "\nFIM_QUEDA\n");
        sceKernelWrite(g_queda_fd, b, n);
    }
    jk_sigaction(sig, &g_velho[sig & 15], 0);                 /* devolve o tratamento original: o jogo cai como sempre */
}
static void instalar_queda(void)
{
    static int feito = 0;
    if (feito) return;
    feito = 1;
    if (existe(PASTA "/sem_queda.txt")) { logf_("QUEDA desligado (sem_queda.txt)"); return; }
    g_queda_fd = sceKernelOpen(PASTA "/queda.log", 0x0001 | 0x0200 | 0x0008, 0666);
    static const int sinais[5] = {4, 6, 8, 10, 11};            /* ILL ABRT FPE BUS SEGV */
    int ok = 0;
    for (int i = 0; i < 5; i++) {
        struct jk_sigact a; memset(&a, 0, sizeof a);
        a.handler = (void *)queda; a.flags = JK_SA_SIGINFO;
        if (jk_sigaction(sinais[i], &a, &g_velho[sinais[i] & 15]) == 0) ok++;
    }
    logf_("QUEDA registro instalado sinais=%d/5 fd=%d", ok, g_queda_fd);
}

/* ---- chaveamento (v1.06) ----
 * queda.log da v1.04c: o jogo cai em eboot 0xd694c0 (funcao da tela de chaveamento, 0xd68fc0).
 * Ela pega "o mata-mata" da copa por dois caminhos e compara:
 *   lista[0] = primeira competicao tipo 3 da regra (0x1b15f80)  -> na Sula: a primeira fase (0xe)
 *   ax       = fase atual da copa (0x1b23370)                     -> na Sula: as eliminatorias (0xd)
 *   0xd69053: 66 3b 01   cmp (%rcx),%ax
 *   0xd69056: 75 10      jne -> r14 = NULL  (e depois le r14 -> queda)
 * Correcao: usar sempre a fase atual -> grava ax em lista[0] e segue:
 *   0xd69053: 66 89 01   mov %ax,(%rcx)
 *   0xd69056: 90 90      nop nop
 * Na Libertadores e nas outras copas os dois ja sao iguais: nada muda para elas. */
typedef struct { uint64_t rva; uint8_t n; uint8_t orig[9]; uint8_t novo[9]; const char *nome; const char *desliga; } remendo_t;
static const remendo_t g_remendos[2] = {
    /* tela de chaveamento: usa sempre a fase atual da copa (era: ponteiro nulo -> queda) */
    { 0xd69053ULL, 5, {0x66,0x3b,0x01,0x75,0x10}, {0x66,0x89,0x01,0x90,0x90}, "CHAVEAMENTO", "sem_chaveamento.txt" },
    /* colunas do chaveamento (0xd6908b): a regra 6 (Sula) passa a usar a lista de 5 fases
     * (playoff, oitavas, quartas, semi, final) que o jogo ja tem para a regra 3. A regra 2 continua
     * na lista padrao de 4 fases, como antes:  cmp $2 / je padrao  ->  cmp $6 / je 5_fases */
    { 0xd6908bULL, 9, {0x83,0xf8,0x02,0x0f,0x84,0xa4,0x00,0x00,0x00}, {0x83,0xf8,0x06,0x0f,0x84,0x09,0x00,0x00,0x00}, "OITAVAS", "sem_oitavas.txt" },
};
static int ler_eboot(uint64_t src, void *dest, uint64_t n)
{
    struct proc_rw rw;
    memset(&rw, 0, sizeof rw);
    rw.address = src; rw.data = dest; rw.length = n; rw.write_flags = 0;
    return sys_sdk_proc_rw(&rw) == 0;
}
static void aplicar_remendo(const remendo_t *r)
{
    char sw[96]; snprintf(sw, sizeof sw, PASTA "/%s", r->desliga);
    if (existe(sw)) { logf_("%s desligado (%s)", r->nome, r->desliga); return; }
    uint64_t a = g_base + r->rva;
    uint8_t b[9] = {0};
    if (!ler_eboot(a, b, r->n)) { logf_("%s recusado=leitura", r->nome); return; }
    if (memcmp(b, r->novo, r->n) == 0) { logf_("%s ja aplicado", r->nome); return; }
    if (memcmp(b, r->orig, r->n) != 0) {
        logf_("%s recusado=bytes_diferentes (%02x %02x %02x %02x %02x)", r->nome, b[0], b[1], b[2], b[3], b[4]);
        return;
    }
    struct proc_rw rw;
    memset(&rw, 0, sizeof rw);
    rw.address = a; rw.data = (void *)r->novo; rw.length = r->n; rw.write_flags = 1;
    if (sys_sdk_proc_rw(&rw) != 0) { logf_("%s recusado=gravacao", r->nome); return; }
    memset(b, 0, sizeof b);
    int ok = ler_eboot(a, b, r->n) && memcmp(b, r->novo, r->n) == 0;
    logf_("%s %s rva=0x%llx", r->nome, ok ? "ok" : "conferencia_falhou", (unsigned long long)r->rva);
}
static void corrigir_chaveamento(void)
{
    for (int i = 0; i < (int)(sizeof g_remendos / sizeof g_remendos[0]); i++) aplicar_remendo(&g_remendos[i]);
}

static campeoes_t g_camp;
/* v1.12: o byte 0xd69054 escolhe qual fase a tela de chaveamento usa quando as duas buscas discordam:
 *   0x89 (mov %ax,(%rcx))  -> usa a fase ATUAL   (certo quando o mata-mata da Sula esta rolando)
 *   0x8b (mov (%rcx),%ax)  -> usa a 1a fase tipo 3 da copa (certo antes disso: mostra a primeira fase)
 * Com a fase atual nos grupos, a versao 0x89 apontava para a fase-mae dos grupos e o jogo caia. */
static int g_chave_modo = -1;
static void ajustar_chaveamento(uint64_t db, int ano)
{
    if (existe(PASTA "/sem_chaveamento.txt")) return;
    int ativo = 0;
    uint32_t n = U32(db + DB_COMP_N);
    for (uint32_t i = 0; i < n && i < 400; i++) {
        uint64_t cp = db + DB_COMP_TAB + (uint64_t)i * COMP_STRIDE;
        if (U32(cp + 0xf4) != SULA_REGRA || U32(cp + 0xf8) != 3) continue;
        if (U32(cp + 0xfc) == 0xffffffff || U32(cp + 0x100) == 0xffffffff) continue;   /* so o mata-mata (2+ rodadas) */
        uint32_t r0 = U32(cp + 0xfc); if (r0 > 0x2000) continue;
        uint64_t r = db + DB_CONFRONTOS + (uint64_t)r0 * RODADA_STRIDE;
        int nconf = *(volatile uint8_t *)(r + 0x204);
        if (nconf <= 0 || nconf > 16) continue;
        uint32_t id = U32(r + 4 + 8) & 0xffff;                     /* 1o jogo do 1o confronto */
        if (id >= JOGOS_MAX) continue;
        uint64_t g = db + DB_JOGOS + (uint64_t)id * JOGO_STRIDE;
        if (U16(g) == id && U16(g + 8) == ano) ativo = 1;           /* mata-mata publicado nesta temporada */
    }
    uint8_t quer = ativo ? 0x89 : 0x8b;
    if (g_chave_modo == quer) return;
    uint64_t a = g_base + 0xd69054ULL;
    uint8_t b[4] = {0};
    if (!ler_eboot(a - 1, b, 4) || b[0] != 0x66 || b[2] != 0x01 || b[3] != 0x90 || (b[1] != 0x89 && b[1] != 0x8b)) {
        if (g_chave_modo != -2) logf_("CHAVEAMENTO modo: bytes inesperados (%02x %02x %02x %02x), nao mexo", b[0], b[1], b[2], b[3]);
        g_chave_modo = -2; return;
    }
    if (b[1] != quer) {
        struct proc_rw rw; memset(&rw, 0, sizeof rw);
        rw.address = a; rw.data = &quer; rw.length = 1; rw.write_flags = 1;
        if (sys_sdk_proc_rw(&rw) != 0) { logf_("CHAVEAMENTO modo: gravacao recusada"); return; }
    }
    g_chave_modo = quer;
    logf_("CHAVEAMENTO modo=%s", ativo ? "fase_atual (mata-mata da Sula rolando)" : "primeira_fase (antes do mata-mata)");
}

static int g_camp_ano = 0, g_camp_ok = 0;

static int tem_jogo(uint64_t db, uint16_t chave, int ano)
{
    for (int i = 0; i < JOGOS_MAX; i++) {
        uint64_t j = db + DB_JOGOS + (uint64_t)i * JOGO_STRIDE;
        if (U16(j + 2) == chave && U16(j + 8) == ano) return 1;
    }
    return 0;
}

static void ciclo(uint64_t db)
{
    int hoje = U16(db + DB_DATA);
    int ano = U16(db + DB_DATA + 2);
    if (ano < 2000 || ano > 2100 || hoje > 365) return;   /* carreira ainda nao comecou */

    acompanhar(db, ano, hoje);
    chaves_sula(db);
    ajustar_chaveamento(db, ano);

    if (g_camp_ano != ano) {                               /* le os campeoes da temporada anterior 1x */
        g_camp_ano = ano;
        g_camp_ok = ler_campeoes(ano - 1, &g_camp);
        if (g_camp_ok && !g_camp.sula) {                   /* final da Sula decidida depois do arquivo: pega do plugin do RB */
            int s = sula_do_rb(ano - 1);
            if (s) {
                g_camp.sula = s; gravar_campeoes(ano - 1, &g_camp);
                logf_("CAMPEOES ano=%d sula=%d (do campeoes.txt do plugin da Sula; arquivo corrigido)", ano - 1, s);
            }
        }
        if (!g_camp_ok) logf_("SEM_CAMPEOES de %d: as duas copas ficam com o nome e os clubes originais nesta temporada", ano - 1);
        g_copas[0].feito_ano = g_copas[1].feito_ano = 0;
    }
    campeoes_t *c = &g_camp;
    int b_sup = (c->cdb == c->br) ? c->vice_br : c->cdb;
    int ok_sup = g_camp_ok && c->br && b_sup && c->br != b_sup;
    int ok_rec = g_camp_ok && c->lib && c->sula && c->lib != c->sula;
    renomear(db, g_copas[0].chave, ok_sup ? g_copas[0].nome : g_copas[0].nome_original);
    renomear(db, g_copas[1].chave, ok_rec ? g_copas[1].nome : g_copas[1].nome_original);
    int dezembro = hoje >= DIA_VOLTA_CATEG;                 /* virada da temporada: categoria original */
    static int sem_cat = -1, conta = 0;
    if (sem_cat < 0 || ++conta >= 15) {                     /* confere o arquivo a cada ~30 s */
        conta = 0;
        int s2 = existe(PASTA "/sem_categoria.txt");
        if (s2 != sem_cat) logf_(s2 ? "CATEGORIA desligada (sem_categoria.txt): copas na categoria original" : "CATEGORIA ligada");
        sem_cat = s2;
    }
    if (sem_cat) dezembro = 1;
    /* v1.13: so muda a aba depois que o jogo da copa ja existe no calendario da temporada. Numa ML nova o
     * calendario e gerado depois que o plugin ja esta rodando; com a Recopa ja nos internacionais (categoria 1)
     * o jogo NAO cria a partida da Super Taca Colombiana. Com a categoria original ele cria, e ai a gente troca. */
    categoria(db, &g_copas[0], ok_sup && !dezembro && tem_jogo(db, g_copas[0].chave, ano));
    categoria(db, &g_copas[1], ok_rec && !dezembro && tem_jogo(db, g_copas[1].chave, ano));
    static uint32_t tiradas_log = 0;
    if (g_lista_tiradas != tiradas_log && (tiradas_log == 0 || g_lista_tiradas >= tiradas_log * 10)) {
        tiradas_log = g_lista_tiradas;
        logf_("LISTA_LIGA nossas copas fora da lista da liga vezes=%u", tiradas_log);
    }
    if (ok_sup) aplicar(db, &g_copas[0], ano, hoje, c->br, b_sup);
    if (ok_rec) aplicar(db, &g_copas[1], ano, hoje, c->lib, c->sula);
}

/* ---------- v1.17: licenca assinada (mesmo esquema do jk_check_var v4.16) ---------- */
#define ARQ_DEV_LIC   "/data/jkpatchs_dev.txt"
#define ARQ_LIC_SUP   "/data/jk/lic/supercopas.lic"
#define ARQ_VISTO_SUP "/data/jk/lic/.supercopas_t"
#define VERSAO_TXT_SUP "1.17"
static int ler_texto_lic(const char *p, char *buf, int cap)
{
    int fd = sceKernelOpen(p, 0, 0);
    if (fd < 0) return -1;
    int n = (int)sceKernelRead(fd, buf, (size_t)(cap - 1));
    sceKernelClose(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return n;
}
static int checar_licenca(void)
{
#ifdef JK_LIC_PUB_PENDENTE
    logf_("build sem chave publica (source/lic_pub.h provisorio)");
    return JK_LIC_ASSINATURA;
#else
    char dev[80];
    int nd = ler_texto_lic(ARQ_DEV_LIC, dev, sizeof dev);
    if (nd < 32) return JK_LIC_FORMATO;
    dev[32] = 0;
    for (int i = 0; i < 32; i++) if (dev[i] >= 'A' && dev[i] <= 'F') dev[i] = (char)(dev[i] + 32);
    static char lic[512];
    int nl = ler_texto_lic(ARQ_LIC_SUP, lic, sizeof lic);
    if (nl <= 0) return JK_LIC_FORMATO;
    OrbisKernelTimeval tv;
    memset(&tv, 0, sizeof tv);
    sceKernelGettimeofday(&tv);
    uint64_t agora = tv.tv_sec > 0 ? (uint64_t)tv.tv_sec : 0, visto = 0, vence = 0;
    char vb[32];
    int nv = ler_texto_lic(ARQ_VISTO_SUP, vb, sizeof vb);
    for (int i = 0; i < nv && vb[i] >= '0' && vb[i] <= '9'; i++) visto = visto * 10 + (uint64_t)(vb[i] - '0');
    int r = jk_lic_checar(lic, (size_t)nl, JK_LIC_PUB, "supercopas", VERSAO_TXT_SUP, dev, agora, visto, &vence);
    if (r == JK_LIC_OK) {
        if (agora > visto) {
            int fd = sceKernelOpen(ARQ_VISTO_SUP, 0x0001 | 0x0200 | 0x0400, 0666);
            if (fd >= 0) { char t[24]; int k = snprintf(t, sizeof t, "%llu", (unsigned long long)agora); if (k > 0) sceKernelWrite(fd, t, (size_t)k); sceKernelClose(fd); }
        }
        logf_("licenca ok (vale por mais %llu h)", (unsigned long long)((vence - agora) / 3600));
    }
    return r;
#endif
}

static void *trabalho(void *arg)
{
    (void)arg;
    sceKernelMkdir("/data/jk", 0777);
    sceKernelMkdir(PASTA, 0777);
    struct proc_info pi; memset(&pi, 0, sizeof pi);
    if (sys_sdk_proc_info(&pi) != 0) { logf_("ERRO proc_info"); return 0; }
    g_base = pi.base_address;
    logf_("JK_SUPERCOPAS v1.16 titulo=%s versao=%s", pi.titleid, pi.version);
    /* v1.16: Demisan (CUSA22062, 01.03) tem o executavel igual ao do Mega nos enderecos que usamos
     * (conferido com a copia da sonda: funcoes do chaveamento, menu e carreira identicas; os remendos
     * ainda conferem os bytes antes de gravar). */
    if (strncmp(pi.titleid, "CUSA18676", 9) != 0 && strncmp(pi.titleid, "CUSA22062", 9) != 0) { logf_("titulo diferente: parado"); return 0; }

    {   /* v1.17: sem licenca valida, nao instala nenhum gancho (so sai) */
        int lic = checar_licenca();
        if (lic != JK_LIC_OK) {
            logf_("SUPERCOPAS desligado: %s", jk_lic_motivo(lic));
            return 0;
        }
    }

    uint64_t ultimo = 0;
    while (g_rodando) {
        sceKernelUsleep(2000000);
        uint64_t db = carreira();
        if (!db) { if (ultimo) { g_copas[0].feito_ano = g_copas[1].feito_ano = 0; g_camp_ano = 0; ultimo = 0; } continue; }
        ultimo = db;
        static int tentou_gancho = 0, espera = 0;
        if (!tentou_gancho && ++espera >= 3) { instalar_queda(); corrigir_chaveamento(); tentou_gancho = 1; logf_("GANCHO_LISTA passo=inicio"); instalar_lista(); }
        ciclo(db);
    }
    return 0;
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    scePthreadCreate(&g_thr, 0, trabalho, 0, "jk_supercopas");   /* igual a v1.02: nada mais aqui */
    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    g_rodando = 0;
    desinstalar_lista();
    return 0;
}

int32_t attr_module_hidden module_start(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
int32_t attr_module_hidden module_stop(size_t argc, const void *args) { (void)argc; (void)args; return 0; }

/* comeco original da funcao do jogo (17 bytes) + salto de volta para eboot+0xfee101 */
__attribute__((naked, noinline, used)) static void *jk_lista_orig(void *saida, uint32_t liga)
{
    __asm__ volatile(
        "push %rbp\n"
        "mov %rsp,%rbp\n"
        "push %r15\n"
        "push %r14\n"
        "push %r13\n"
        "push %r12\n"
        "push %rbx\n"
        "sub $0x78,%rsp\n"
        "jmp *g_lista_volta(%rip)\n");
}
