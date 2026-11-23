/*
 * JK CRAQUE v1.03 - PES 2021 (Mega/Brasfoot CUSA18676 01.08 e Demisan CUSA22062)
 *
 * "Craque do Jogo": a cerimonia de melhor da partida (JK_CRAQUE.cpk) tem dois visuais no mesmo CPK:
 *   - Craque Betano  (Brasileirao, Copa do Brasil e o resto): pastas OP-04_POTM1_TROPHY, OP-04_BOARD_POTM1, OP-04_BOARD_1
 *   - Best of the Match (Libertadores): pastas OP-04_POTM2_TROPHY, OP-04_BOARD_POTM2, OP-04_BOARD_2
 * As cenas de fim de jogo sempre pedem as pastas "1". Este plugin entra na funcao do jogo que executa os pedidos
 * de arquivo (eboot 0x1a522e0, a mesma que grava "[%s] file read error (criFsLoader_GetStatus...)") e, quando a
 * partida e da Libertadores, troca no caminho pedido POTM1 -> POTM2 e BOARD_1 -> BOARD_2 (mesmo tamanho, no lugar).
 * Competicao atual: int em eboot+0x5bff80c (a mesma que o jk_licencas le).
 *
 * Config (opcional): /data/jk/craque.txt   linha  lib=5   (ids separados por virgula que usam o visual 2)
 * Desligar: /data/jk/craque_off.txt
 * v1.03: a competicao certa vem do mesmo lugar que o jk_licencas usa: *( *( *(eboot+0x5b4e398) +0x60) +0x358d0).
 *        O int em eboot+0x5bff80c (usado ate a v1.02) e a LICENCA do placar (6 = Brazil), nao a competicao;
 *        fica so como reserva quando o ponteiro nao existe (Brazil/Spain -> Betano, Italy -> Libertadores).
 * v1.02: 3 visuais: 1 Craque Betano (betano=21,24 Brasileirao e Copa do Brasil), 2 Libertadores (lib=5),
 *        3 JK PATCHS (todas as outras competicoes e partidas): pastas OP-04_POTM3_TROPHY, OP-04_BOARD_POTM3, OP-04_BOARD_3.
 *        craque.txt aceita as linhas  lib=5  e  betano=21,24
 * v1.01: na hora da cena de fim de jogo a competicao ja volta a 0; o plugin guarda a ultima competicao
 *        diferente de 0 vista durante a partida (amostra a cada 0,2 s) e usa essa.
 * Log: /data/jk/craque.log  (caminhos das pastas OP-04 pedidos, competicao e se trocou)
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

attr_public const char *g_pluginName = "jk_craque";
attr_public const char *g_pluginDesc = "JK PATCHS - Craque do Jogo por competicao";
attr_public const char *g_pluginAuth = "JK PATCHS";
attr_public uint32_t g_pluginVersion = 0x00000103;

#define RVA_ARQUIVO   0x1a522e0ULL     /* executa um pedido de arquivo: rdi = pedido, caminho (std::string) em +0x20 */
#define RVA_LICENCA   0x5bff80cULL     /* int: licenca do placar (0 Pes,1 Ucl,2 England,3 France,4 Italy,5 Spain,6 Brazil,7 Acl,8 Euro) */
#define RVA_CARREIRA  0x5b4e398ULL     /* ponteiro; +0x60 -> partida; +0x358d0 = competicao (como o jk_licencas) */
#define PASTA         "/data/jk"

static uint64_t g_base;
static volatile int g_rodando = 1;
static OrbisPthread g_thr;
static int g_libs[16] = {5}; static int g_nlibs = 1;
static int g_bets[16] = {21, 24}; static int g_nbets = 2;
int jk_qmp(void *addr, void **start, void **end, int *prot) __asm__("sceKernelQueryMemoryProtection");
static int legivel(uint64_t a, uint64_t n)
{
    if (a < 0x10000) return 0;
    void *st = 0, *e = 0; int p = 0;
    if (jk_qmp((void *)a, &st, &e, &p) != 0 || !(p & 1)) return 0;
    return (uint64_t)e >= a + n;
}
static volatile int g_comp_partida = 0x7fffffff;   /* ultima competicao != 0 vista */
static volatile int g_fonte = 0;                     /* 1 = competicao real, 2 = reserva pela licenca */

/* competicao da partida: ponteiro do jk_licencas; sem ele, -1000-licenca (reserva) */
static int ler_competicao(void)
{
    uint64_t p = g_base + RVA_CARREIRA;
    if (legivel(p, 8)) {
        uint64_t o = *(volatile uint64_t *)p;
        if (o && legivel(o + 0x60, 8)) {
            uint64_t m = *(volatile uint64_t *)(o + 0x60);
            if (m && legivel(m + 0x358d0, 4)) { g_fonte = 1; return *(volatile int32_t *)(m + 0x358d0); }
        }
    }
    g_fonte = 2;
    return -1000 - *(volatile int32_t *)(g_base + RVA_LICENCA);
}

__attribute__((visibility("hidden"))) uint64_t g_arq_volta;
static uint64_t jk_arq_orig(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f);

/* ---------- log ---------- */
static int g_log = -1;
static void logf_(const char *fmt, ...)
{
    char b[600]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    if (g_log < 0) g_log = sceKernelOpen(PASTA "/craque.log", 0x0001 | 0x0200 | 0x0400, 0666);   /* novo a cada jogo */
    if (g_log >= 0) sceKernelWrite(g_log, b, n);
}
static int existe(const char *p) { OrbisKernelStat st; return sceKernelStat(p, &st) == 0; }

/* ---------- anel de caminhos vistos (o gancho nao escreve arquivo; a thread grava) ---------- */
#define ANEL 32
static char g_anel[ANEL][160];
static int  g_anel_comp[ANEL];
static int  g_anel_troca[ANEL];
static volatile uint32_t g_anel_pos = 0, g_anel_lido = 0;
static volatile uint32_t g_trocas = 0, g_vistos = 0;

static int tem_op04(const char *p, uint64_t n)
{
    for (uint64_t i = 0; i + 6 <= n; i++) if (p[i] == 'O' && p[i+1] == 'P' && p[i+2] == '-' && p[i+3] == '0' && p[i+4] == '4' && p[i+5] == '_') return 1;
    return 0;
}
static int eh_lib(int c) { for (int i = 0; i < g_nlibs; i++) if (g_libs[i] == c) return 1; return 0; }
static int eh_bet(int c) { for (int i = 0; i < g_nbets; i++) if (g_bets[i] == c) return 1; return 0; }
static char visual(int c)
{
    if (c <= -1000) { int l = -1000 - c; return l == 4 ? '2' : (l == 6 || l == 5) ? '1' : '3'; }   /* reserva: licenca */
    return eh_lib(c) ? '2' : eh_bet(c) ? '1' : '3';
}
static const char *nome_visual(char v) { return v == '2' ? "LIBERTADORES" : v == '1' ? "BETANO" : "JK PATCHS"; }

/* troca no lugar (mesmo tamanho): POTMx e BOARD_x -> visual (1, 2 ou 3), so dentro de pastas "OP-04_" */
static int ajustar(char *p, uint64_t n, char para)
{
    int mudou = 0;
    for (uint64_t i = 0; i + 6 <= n; i++) {
        if (p[i] != 'O' || memcmp(p + i, "OP-04_", 6) != 0) continue;
        uint64_t j = i + 6;
        while (j < n && p[j] != '/' && p[j] != 0) {
            if (j + 5 <= n && memcmp(p + j, "POTM", 4) == 0 && p[j + 4] >= '1' && p[j + 4] <= '3' && p[j + 4] != para) { p[j + 4] = para; mudou = 1; }
            if (j + 7 <= n && memcmp(p + j, "BOARD_", 6) == 0 && p[j + 6] >= '1' && p[j + 6] <= '3' && (j + 7 == n || p[j + 7] == '/' || p[j + 7] == 0) && p[j + 6] != para) { p[j + 6] = para; mudou = 1; }
            j++;
        }
        i = j;
    }
    return mudou;
}

static uint64_t arquivo_hook(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    uint8_t *req = (uint8_t *)a;
    if (req) {
        uint64_t cap = *(volatile uint64_t *)(req + 0x38);
        uint64_t len = *(volatile uint64_t *)(req + 0x30);
        char *p = cap >= 0x10 ? *(char **)(req + 0x20) : (char *)(req + 0x20);
        if (p && len > 6 && len < 4096 && tem_op04(p, len)) {
            int agora = ler_competicao();
            if (agora != 0) g_comp_partida = agora;
            int comp = g_comp_partida;
            int t = ajustar(p, len, visual(comp));
            if (t) g_trocas++;
            g_vistos++;
            uint32_t k = g_anel_pos % ANEL;
            uint64_t m = len < 159 ? len : 159;
            memcpy(g_anel[k], p, m); g_anel[k][m] = 0;
            g_anel_comp[k] = comp; g_anel_troca[k] = t; g_anel_pos++;
        }
    }
    return jk_arq_orig(a, b, c, d, e, f);
}

static int gravar_eboot(uint64_t dest, const void *src, uint64_t n)
{
    struct proc_rw rw; memset(&rw, 0, sizeof rw);
    rw.address = dest; rw.data = (void *)src; rw.length = n; rw.write_flags = 1;
    if (sys_sdk_proc_rw(&rw) != 0) return 0;
    return memcmp((const void *)dest, src, n) == 0;
}

static const uint8_t g_esperado[17] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xe4,0xe0};
static int g_gancho = 0;

static void instalar(void)
{
    uint64_t f = g_base + RVA_ARQUIVO;
    const volatile uint8_t *bb = (const volatile uint8_t *)f;
    for (int i = 0; i < 17; i++) if (bb[i] != g_esperado[i]) {
        logf_("GANCHO recusado: bytes diferentes em 0x%llx (%02x %02x %02x %02x) - outra versao do jogo?", (unsigned long long)RVA_ARQUIVO, bb[0], bb[1], bb[2], bb[3]);
        return;
    }
    g_arq_volta = f + 17;
    uint8_t salto[17]; uint64_t alvo = (uint64_t)(void *)arquivo_hook;
    salto[0] = 0xff; salto[1] = 0x25; salto[2] = salto[3] = salto[4] = salto[5] = 0;
    memcpy(salto + 6, &alvo, 8); salto[14] = salto[15] = salto[16] = 0x90;
    if (!gravar_eboot(f, salto, sizeof salto)) { gravar_eboot(f, g_esperado, 17); logf_("GANCHO recusado: gravacao"); return; }
    g_gancho = 1;
    logf_("GANCHO ok eboot+0x%llx", (unsigned long long)RVA_ARQUIVO);
}

static int ler_lista(const char *t, const char *chave, int *v, int max)
{
    const char *p = strstr(t, chave);
    if (!p) return -1;
    p += strlen(chave); int n = 0;
    while (*p && *p != '\n' && *p != '\r' && n < max) {
        int neg = 0, x = 0, tem = 0;
        if (*p == '-') { neg = 1; p++; }
        while (*p >= '0' && *p <= '9') { x = x * 10 + (*p - '0'); p++; tem = 1; }
        if (tem) v[n++] = neg ? -x : x;
        if (*p == ',' || *p == ' ') p++; else if (!tem) break;
    }
    return n;
}

static void ler_config(void)
{
    int fd = sceKernelOpen(PASTA "/craque.txt", 0, 0);
    if (fd >= 0) {
        char t[512]; int n = sceKernelRead(fd, t, sizeof t - 1); sceKernelClose(fd);
        if (n < 0) n = 0;
        t[n] = 0;
        int a[16], k;
        if ((k = ler_lista(t, "lib=", a, 16)) > 0) { memcpy(g_libs, a, k * sizeof(int)); g_nlibs = k; }
        if ((k = ler_lista(t, "betano=", a, 16)) >= 0) { memcpy(g_bets, a, (k > 0 ? k : 0) * sizeof(int)); g_nbets = k; }
    }
    char s1[128], s2[128]; int k1 = 0, k2 = 0; s1[0] = s2[0] = 0;
    for (int i = 0; i < g_nlibs; i++) k1 += snprintf(s1 + k1, sizeof s1 - k1, "%s%d", i ? "," : "", g_libs[i]);
    for (int i = 0; i < g_nbets; i++) k2 += snprintf(s2 + k2, sizeof s2 - k2, "%s%d", i ? "," : "", g_bets[i]);
    logf_("CONFIG %s lib=%s betano=%s (resto: JK PATCHS)", fd >= 0 ? "craque.txt" : "padrao", s1, s2);
}

static void *trabalho(void *arg)
{
    (void)arg;
    sceKernelMkdir(PASTA, 0777);
    struct proc_info pi; memset(&pi, 0, sizeof pi);
    if (sys_sdk_proc_info(&pi) != 0) { logf_("ERRO proc_info"); return 0; }
    g_base = pi.base_address;
    logf_("JK_CRAQUE v1.03 titulo=%s versao=%s", pi.titleid, pi.version);
    if (strncmp(pi.titleid, "CUSA18676", 9) != 0 && strncmp(pi.titleid, "CUSA22062", 9) != 0) { logf_("titulo diferente: parado"); return 0; }
    if (existe(PASTA "/craque_off.txt")) { logf_("DESLIGADO (craque_off.txt): o jogo pede sempre as pastas 1 (Craque Betano)"); return 0; }
    ler_config();
    sceKernelUsleep(3000000);              /* deixa o jogo subir antes de gravar no executavel */
    instalar();
    int comp_ant = 0x7fffffff, part_ant = 0x7fffffff, tic = 0;
    while (g_rodando) {
        sceKernelUsleep(200000);
        int comp = ler_competicao();
        if (comp != 0) g_comp_partida = comp;
        if (comp != comp_ant) { logf_("t=%ds COMPETICAO agora=%d (%s) licenca=%d", tic / 5, comp, g_fonte == 1 ? "partida" : "reserva pela licenca", *(volatile int32_t *)(g_base + RVA_LICENCA)); comp_ant = comp; }
        if (g_comp_partida != part_ant) { part_ant = g_comp_partida; logf_("t=%ds PARTIDA competicao=%d -> visual %s", tic / 5, part_ant, nome_visual(visual(part_ant))); }
        tic++;
        while (g_anel_lido != g_anel_pos) {
            if (g_anel_pos - g_anel_lido > ANEL) g_anel_lido = g_anel_pos - ANEL;
            uint32_t k = g_anel_lido % ANEL;
            logf_("ARQ comp=%d %s %s", g_anel_comp[k], g_anel_troca[k] ? "TROCADO" : "igual", g_anel[k]);
            g_anel_lido++;
        }
    }
    return 0;
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    scePthreadCreate(&g_thr, 0, trabalho, 0, "jk_craque");
    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    g_rodando = 0;
    if (g_gancho) { gravar_eboot(g_base + RVA_ARQUIVO, g_esperado, 17); g_gancho = 0; }
    return 0;
}

int32_t attr_module_hidden module_start(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
int32_t attr_module_hidden module_stop(size_t argc, const void *args) { (void)argc; (void)args; return 0; }

/* comeco original da funcao do jogo (17 bytes) + salto de volta para eboot+0x1a522f1 */
__attribute__((naked, noinline, used)) static uint64_t jk_arq_orig(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    __asm__ volatile(
        "push %rbp\n"
        "mov %rsp,%rbp\n"
        "push %r15\n"
        "push %r14\n"
        "push %r13\n"
        "push %r12\n"
        "push %rbx\n"
        "and $0xffffffffffffffe0,%rsp\n"
        "jmp *g_arq_volta(%rip)\n");
}
