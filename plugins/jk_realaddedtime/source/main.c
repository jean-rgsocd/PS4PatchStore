/*
 * JK REAL ADDED TIME - PES 2021 (CUSA18676) - v1.00
 *
 * Port of the PC RealAddedTime v3.2 mod (Sider/Lua) to PS4 GoldHEN PRX.
 * Follows jk_craque pattern: naked trampolines, manual 14-byte JMP, sys_sdk_proc_rw.
 *
 * HOOK POINTS (confirmed via RE on eboot_CUSA18676.bin v01.08, base 0x400000):
 *   1. Scoreboard snapshot: RVA 0x9B1FA0 (20 bytes prologue, complete instructions)
 *      - RDI = HUD pointer, reads announced added time at +0x118
 *   2. Referee time-up check: RVA 0xB54430 (26 bytes prologue, complete instructions)
 *      - Decides if match time is up, calls TimeUpDialog display
 *   3. MatchMain global: getter at 0x2873A0 loads MatchMain+0x40 -> match_data*
 *      - Global pointer at 0x568D708 (in .bss)
 *      - match_data offsets: period, match_time, score_home, score_away (TBD)
 *   4. HUD display offset: +0x118 confirmed, write offset TBD
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

attr_public const char *g_pluginName = "jk_realaddedtime";
attr_public const char *g_pluginDesc = "JK PATCHS - Real Added Time (acrescimos realistas)";
attr_public const char *g_pluginAuth = "JK PATCHS";
attr_public uint32_t g_pluginVersion = 0x00000100;

/* ------------------------------------------------------------------------- */
/* FORWARD DECLARATIONS                                                      */
/* ------------------------------------------------------------------------- */
static int ler_mem(uint64_t addr, void *dst, size_t len);
static int escrever_mem(uint64_t addr, const void *src, size_t len);
static int legivel(uint64_t a, uint64_t n);
static void dump_match_data(uint64_t ptr);
static int calcular_acrescimo(void);
static void scan_match_state(void);
static void *trabalho(void *arg);

/* ------------------------------------------------------------------------- */
/* CONFIG - RVAs CONFIRMADOS NO EBOOT 01.08 (base 0x400000)                 */
/* ------------------------------------------------------------------------- */
static uint64_t g_base = 0;

/* Scoreboard: RVA 0x9B1FA0 - prologue 20 bytes (complete instructions)
 * EBOOT bytes: 55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 48 41 89 cf
 * push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x48; mov r15d,ecx */
#define RVA_SCOREBOARD_SNAPSHOT   0x9B1FA0ULL
#define SCOREBOARD_PROLOGO_SIZE   20

/* Referee time-up: RVA 0xB54430 - prologue 26 bytes (complete instructions)
 * EBOOT bytes: 55 48 89 e5 41 57 41 56 53 48 83 ec 38 4c 8b 3d 6c ab aa 03 48 89 fb 49 8b 07
 * push rbp; mov rbp,rsp; push r15; push r14; push rbx; sub rsp,0x38;
 * mov r15,[rip+0x3AAAB6C] -> 0x568D708; mov rbx,rdi; mov rax,[r15] */
#define RVA_REFEREE_TIME_UP       0xB54430ULL
#define REFEREE_PROLOGO_SIZE      26

/* MatchMain global pointer at 0x568D708 (in .bss, zero-filled on disk)
 * Getter at 0x2873A0: mov rax, [rip+0x568D708]; mov rax, [rax]; mov rax, [rax+0x40] */
#define RVA_MATCH_MANAGER_PTR     0x568D708ULL  /* Global pointer to MatchMain */
#define MGR_OFF_MATCH_DATA_PTR    0x40          /* MatchMain+0x40 -> match_data* */

/* MatchData offsets - TO BE CONFIRMED via live testing */
#define MGR_OFF_PERIOD            0x0ULL        /* TODO: find offset */
#define MGR_OFF_MATCH_TIME        0x0ULL        /* TODO: find offset */
#define MGR_OFF_SCORE_HOME        0x0ULL        /* TODO: find offset */
#define MGR_OFF_SCORE_AWAY        0x0ULL        /* TODO: find offset */
#define MGR_OFF_STOPPAGES         0x0ULL        /* TODO: find offset */

/* HUD offsets */
#define HUD_OFF_ANNOUNCED_ADDED   0x118         /* Confirmed from PC mod */
#define HUD_OFF_DISPLAY_ADDED     0x0ULL        /* TODO: find write offset */

/* ------------------------------------------------------------------------- */
/* CONSTANTS (from PC RealAddedTime v3.2)                                   */
/* ------------------------------------------------------------------------- */
#define BASE_ADDED_TIME           60
#define MAX_ADDED_TIME            600
#define MIN_ADDED_TIME            30
#define GOAL_TIME                 60
#define FOUL_TIME                 20
#define STOP_TIME                 20
#define RESTART_TIME              8
#define LATE_GOAL_TIME            60
#define JITTER_RANGE              30

#define STATUS_FOUL_CARD          1
#define STATUS_OFFSIDE_INJURY_SUB 2
#define STATUS_RESTART            5

/* ------------------------------------------------------------------------- */
/* GLOBAL STATE                                                              */
/* ------------------------------------------------------------------------- */
static OrbisPthread g_thr;
static volatile int g_running = 1;
static int g_log = -1;

static volatile int g_fouls = 0;
static volatile int g_stops = 0;
static volatile int g_restarts = 0;
static volatile int g_goals = 0;
static volatile int g_late_goals = 0;

static volatile int g_last_period = 0;
static volatile int g_last_score_home = 0;
static volatile int g_last_score_away = 0;
static volatile uint64_t g_last_match_time = 0;

static volatile uint64_t g_hud_ptr = 0;
static volatile int g_game_announced_added = 0;
static volatile int g_module_added = 0;

static uint32_t g_rand_seed = 0x9E3779B9;

/* Diag log limit (like jk_craque LIMITE_LOG_DIAG) */
#define LIMITE_LOG_DIAG 25
static volatile uint32_t g_diag_scoreboard = 0;
static volatile uint32_t g_diag_referee = 0;

/* Prologue verification flags */
static volatile int g_scoreboard_prologue_ok = 0;
static volatile int g_referee_prologue_ok = 0;

/* ------------------------------------------------------------------------- */
/* LOGGING                                                                   */
/* ------------------------------------------------------------------------- */
#define PASTA "/data/jk/realaddedtime"

static void logf_(const char *fmt, ...)
{
    char b[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    if (g_log < 0) g_log = sceKernelOpen(PASTA "/realaddedtime.log", 0x0001 | 0x0200 | 0x0008, 0666);
    if (g_log >= 0) sceKernelWrite(g_log, b, n);
}

/* Dump 0x200 bytes of match_data for analysis */
static void dump_match_data(uint64_t ptr)
{
    uint8_t buf[0x200];
    if (!ler_mem(ptr, buf, sizeof(buf))) return;
    char hex[2048];
    int pos = 0;
    for (int i = 0; i < 0x200; i++) {
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", buf[i]);
        if ((i & 15) == 15) pos += snprintf(hex + pos, sizeof(hex) - pos, "\n");
    }
    logf_("MATCH_DATA @ 0x%llx:\n%s", (unsigned long long)ptr, hex);
}

/* ------------------------------------------------------------------------- */
/* UTILITIES                                                                 */
/* ------------------------------------------------------------------------- */
static int existe(const char *p)
{
    OrbisKernelStat st;
    return sceKernelStat(p, &st) == 0;
}

static uint32_t rand_next(void)
{
    uint32_t x = g_rand_seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rand_seed = x;
    return x;
}

static int rand_range(int min, int max)
{
    return min + (int)(rand_next() % (uint32_t)(max - min + 1));
}

static int ler_mem(uint64_t addr, void *dst, size_t len)
{
    struct proc_rw rw; memset(&rw, 0, sizeof rw);
    rw.address = addr; rw.data = dst; rw.length = len; rw.write_flags = 0;
    return sys_sdk_proc_rw(&rw) == 0;
}

static int escrever_mem(uint64_t addr, const void *src, size_t len)
{
    struct proc_rw rw; memset(&rw, 0, sizeof rw);
    rw.address = addr; rw.data = (void *)src; rw.length = len; rw.write_flags = 1;
    return sys_sdk_proc_rw(&rw) == 0;
}

/* ------------------------------------------------------------------------- */
/* MEMORY QUERY (from jk_sonda)                                              */
/* ------------------------------------------------------------------------- */
int jk_qmp(void *addr, void **start, void **end, int *prot) __asm__("sceKernelQueryMemoryProtection");

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

/* ------------------------------------------------------------------------- */
/* CORE LOGIC: CALCULATE ADDED TIME                                          */
/* ------------------------------------------------------------------------- */
static int calcular_acrescimo(void)
{
    int added = BASE_ADDED_TIME;
    added += g_goals * GOAL_TIME;
    added += g_fouls * FOUL_TIME;
    added += g_stops * STOP_TIME;
    added += g_restarts * RESTART_TIME;
    added += g_late_goals * LATE_GOAL_TIME;

    added = ((added + 30) / 60) * 60;

    if (added > MAX_ADDED_TIME) added = MAX_ADDED_TIME;
    if (added < MIN_ADDED_TIME) added = MIN_ADDED_TIME;

    added += rand_range(-JITTER_RANGE, JITTER_RANGE);
    if (added < MIN_ADDED_TIME) added = MIN_ADDED_TIME;
    if (added > MAX_ADDED_TIME) added = MAX_ADDED_TIME;

    return added;
}

/* ------------------------------------------------------------------------- */
/* NAKED TRAMPOLINES (jk_craque pattern)                                     */
/* ------------------------------------------------------------------------- */
/* Scoreboard trampoline - 20 bytes original prologue (exact EBOOT bytes) */
__attribute__((naked, noinline, used)) static int32_t jk_scoreboard_orig(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    __asm__ volatile(
        ".byte 0x55\n"                 /* push rbp */
        ".byte 0x48,0x89,0xe5\n"       /* mov rbp, rsp */
        ".byte 0x41,0x57\n"            /* push r15 */
        ".byte 0x41,0x56\n"            /* push r14 */
        ".byte 0x41,0x55\n"            /* push r13 */
        ".byte 0x41,0x54\n"            /* push r12 */
        ".byte 0x53\n"                 /* push rbx */
        ".byte 0x48,0x83,0xec,0x48\n"  /* sub rsp, 0x48 */
        ".byte 0x41,0x89,0xcf\n"       /* mov r15d, ecx */
        "jmp *g_scoreboard_volta(%rip)\n");
}

/* Referee trampoline - semantically equivalent to 26-byte EBOOT prologue
 * Original: mov r15, [rip+0x3AAAB6C] (loads MatchMain* from 0x568D708)
 *           mov rax, [r15]
 * Trampoline runs at plugin address, so RIP-relative displacement is wrong.
 * Use absolute addressing: mov rax, 0x568D708; mov r15, [rax] (13 bytes vs 7)
 * Then: mov rbx, rdi; mov rax, [r15]
 * Return address is f+26 (original function continues after prologue). */
__attribute__((naked, noinline, used)) static int32_t jk_referee_orig(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    __asm__ volatile(
        ".byte 0x55\n"                         /* push rbp */
        ".byte 0x48,0x89,0xe5\n"               /* mov rbp, rsp */
        ".byte 0x41,0x57\n"                    /* push r15 */
        ".byte 0x41,0x56\n"                    /* push r14 */
        ".byte 0x53\n"                         /* push rbx */
        ".byte 0x48,0x83,0xec,0x38\n"          /* sub rsp, 0x38 */
        /* mov rax, 0x568D708 (global pointer address) */
        ".byte 0x48,0xb8,0x08,0xd7,0x68,0x56,0x00,0x00,0x00,0x00\n"
        /* mov r15, [rax] (load MatchMain* from global pointer) */
        ".byte 0x49,0x8b,0x38\n"               /* mov r15, [rax] */
        ".byte 0x48,0x89,0xfb\n"               /* mov rbx, rdi */
        ".byte 0x49,0x8b,0x07\n"               /* mov rax, [r15] */
        "jmp *g_referee_volta(%rip)\n");
}

/* Return addresses (set at hook install) */
__attribute__((visibility("hidden"))) uint64_t g_scoreboard_volta;
__attribute__((visibility("hidden"))) uint64_t g_referee_volta;

/* ------------------------------------------------------------------------- */
/* HOOK: SCOREBOARD SNAPSHOT                                                 */
/* ------------------------------------------------------------------------- */
static int32_t g_scoreboard_hook_installed = 0;

static int32_t hook_scoreboard_snapshot(uint64_t rdi, uint64_t rsi, uint64_t rdx, uint64_t rcx,
                                         uint64_t r8, uint64_t r9)
{
    /* Call original via naked trampoline */
    int32_t result = jk_scoreboard_orig(rdi, rsi, rdx, rcx, r8, r9);

    /* Save HUD pointer and read announced added time */
    if (rdi && legivel(rdi, 0x200)) {
        g_hud_ptr = rdi;
        int announced = 0;
        if (ler_mem(rdi + HUD_OFF_ANNOUNCED_ADDED, &announced, sizeof(announced))) {
            g_game_announced_added = announced;
        }
    }

    /* Write our calculated added time to display */
    if (g_hud_ptr && HUD_OFF_DISPLAY_ADDED && g_module_added > 0) {
        escrever_mem(g_hud_ptr + HUD_OFF_DISPLAY_ADDED, &g_module_added, sizeof(g_module_added));
    }

    /* Limited diag logging */
    if (g_diag_scoreboard < LIMITE_LOG_DIAG) {
        g_diag_scoreboard++;
        logf_("HUD: ptr=0x%llx announced=%d module=%d", (unsigned long long)rdi, g_game_announced_added, g_module_added);
    }

    return result;
}

/* ------------------------------------------------------------------------- */
/* HOOK: REFEREE TIME-UP CHECK                                               */
/* ------------------------------------------------------------------------- */
static int32_t g_referee_hook_installed = 0;

static int32_t hook_referee_time_up(uint64_t rdi, uint64_t rsi, uint64_t rdx, uint64_t rcx,
                                     uint64_t r8, uint64_t r9)
{
    /* Call original via naked trampoline */
    int32_t game_says_time_up = jk_referee_orig(rdi, rsi, rdx, rcx, r8, r9);

    /* Override if our added time is higher */
    if (game_says_time_up && g_module_added > g_game_announced_added) {
        if (g_diag_referee < LIMITE_LOG_DIAG) {
            g_diag_referee++;
            logf_("REF OVERRIDE: game=%d module=%d", g_game_announced_added, g_module_added);
        }
        return 0;
    }

    return game_says_time_up;
}

/* ------------------------------------------------------------------------- */
/* HOOK INSTALLATION (jk_craque pattern)                                     */
/* ------------------------------------------------------------------------- */
#define HOOK_JMP_SIZE 14

static int gravar_eboot(uint64_t dest, const void *src, uint64_t n)
{
    struct proc_rw rw; memset(&rw, 0, sizeof rw);
    rw.address = dest; rw.data = (void *)src; rw.length = n; rw.write_flags = 1;
    if (sys_sdk_proc_rw(&rw) != 0) return 0;
    return memcmp((const void *)dest, src, n) == 0;
}

/* Install hook: verify prologue, write JMP with target address */
static int instalar_hook(uint64_t rva, const uint8_t *esperado, int tam_prologo,
                          uint64_t *volta_out, void *hook_fn, const char *nome)
{
    /* Safety checks */
    if (g_base == 0) {
        logf_("%s: RECUSADO - g_base nao inicializado", nome);
        return 0;
    }
    if (hook_fn == NULL) {
        logf_("%s: RECUSADO - hook_fn NULL", nome);
        return 0;
    }
    if (rva == 0) {
        logf_("%s: RECUSADO - RVA zero", nome);
        return 0;
    }
    if (tam_prologo <= 0 || tam_prologo > 64) {
        logf_("%s: RECUSADO - tam_prologo invalido %d", nome, tam_prologo);
        return 0;
    }

    uint64_t f = g_base + rva;
    const volatile uint8_t *bb = (const volatile uint8_t *)f;

    for (int i = 0; i < tam_prologo; i++) {
        if (bb[i] != esperado[i]) {
            logf_("%s: RECUSADO - bytes diferentes em 0x%llx (%02x %02x %02x %02x ...)",
                  nome, (unsigned long long)rva, bb[0], bb[1], bb[2], bb[3]);
            return 0;
        }
    }

    *volta_out = f + tam_prologo;

    uint8_t salto[HOOK_JMP_SIZE];
    uint64_t alvo = (uint64_t)hook_fn;
    salto[0] = 0xff; salto[1] = 0x25;
    salto[2] = salto[3] = salto[4] = salto[5] = 0;
    memcpy(salto + 6, &alvo, 8);

    if (!gravar_eboot(f, salto, HOOK_JMP_SIZE)) {
        gravar_eboot(f, esperado, tam_prologo);
        logf_("%s: FALHA ao gravar hook", nome);
        return 0;
    }

    logf_("%s: hook OK em eboot+0x%llx", nome, (unsigned long long)rva);
    return 1;
}

static void remover_hook(uint64_t rva, const uint8_t *esperado, int tam_prologo, int *flag, const char *nome)
{
    if (*flag) {
        gravar_eboot(g_base + rva, esperado, tam_prologo);
        *flag = 0;
        logf_("%s: hook removido", nome);
    }
}

/* Expected prologue bytes - SCOREBOARD (20 bytes, exact EBOOT) */
static const uint8_t g_scoreboard_esperado[SCOREBOARD_PROLOGO_SIZE] = {
    0x55, 0x48, 0x89, 0xe5,  /* push rbp; mov rbp, rsp */
    0x41, 0x57, 0x41, 0x56,  /* push r15; push r14 */
    0x41, 0x55, 0x41, 0x54,  /* push r13; push r12 */
    0x53,                    /* push rbx */
    0x48, 0x83, 0xec, 0x48,  /* sub rsp, 0x48 */
    0x41, 0x89, 0xcf         /* mov r15d, ecx */
};

/* Expected prologue bytes - REFEREE (26 bytes, exact EBOOT)
 * 55 48 89 e5 41 57 41 56 53 48 83 ec 38 4c 8b 3d 6c ab aa 03 48 89 fb 49 8b 07 */
static const uint8_t g_referee_esperado[REFEREE_PROLOGO_SIZE] = {
    0x55, 0x48, 0x89, 0xe5,  /* push rbp; mov rbp, rsp */
    0x41, 0x57, 0x41, 0x56,  /* push r15; push r14 */
    0x53,                    /* push rbx */
    0x48, 0x83, 0xec, 0x38,  /* sub rsp, 0x38 */
    0x4c, 0x8b, 0x3d, 0x6c, 0xab, 0xaa, 0x03,  /* mov r15, [rip+0x3AAAB6C] -> 0x568D708 */
    0x48, 0x89, 0xfb,        /* mov rbx, rdi */
    0x49, 0x8b, 0x07         /* mov rax, [r15] */
};

/* ------------------------------------------------------------------------- */
/* MATCH STATE SCANNING (read-only first build)                              */
/* ------------------------------------------------------------------------- */
static void scan_match_state(void)
{
    /* Read MatchMain global pointer from known location (0x568D708) */
    uint64_t matchmain_ptr = 0;
    if (!ler_mem(g_base + RVA_MATCH_MANAGER_PTR, &matchmain_ptr, sizeof(matchmain_ptr))) {
        return;
    }
    if (!matchmain_ptr || !legivel(matchmain_ptr, 0x100)) {
        return;
    }

    /* MatchMain+0x40 -> match_data* */
    uint64_t match_data_ptr = 0;
    if (!ler_mem(matchmain_ptr + MGR_OFF_MATCH_DATA_PTR, &match_data_ptr, sizeof(match_data_ptr))) {
        return;
    }
    if (!match_data_ptr || !legivel(match_data_ptr, 0x200)) {
        return;
    }

    /* Verify prologues once */
    if (!g_scoreboard_prologue_ok) {
        const volatile uint8_t *bb = (const volatile uint8_t *)(g_base + RVA_SCOREBOARD_SNAPSHOT);
        int ok = 1;
        for (int i = 0; i < SCOREBOARD_PROLOGO_SIZE; i++) {
            if (bb[i] != g_scoreboard_esperado[i]) { ok = 0; break; }
        }
        g_scoreboard_prologue_ok = ok;
        logf_("Scoreboard prologue: %s", ok ? "OK" : "MISMATCH");
    }
    if (!g_referee_prologue_ok) {
        const volatile uint8_t *bb = (const volatile uint8_t *)(g_base + RVA_REFEREE_TIME_UP);
        int ok = 1;
        for (int i = 0; i < REFEREE_PROLOGO_SIZE; i++) {
            if (bb[i] != g_referee_esperado[i]) { ok = 0; break; }
        }
        g_referee_prologue_ok = ok;
        logf_("Referee prologue: %s", ok ? "OK" : "MISMATCH");
    }

    /* Dump match_data after prologue verification (read-only mode) */
    if (g_scoreboard_prologue_ok && g_referee_prologue_ok) {
        static int dump_counter = 0;
        if (++dump_counter % 1 == 0) {
            dump_match_data(match_data_ptr);
        }
    }

    /* Read period (offset TBD) */
    int period = 0;
    if (ler_mem(match_data_ptr + MGR_OFF_PERIOD, &period, sizeof(period))) {
        if (period != g_last_period && period > 0) {
            logf_("PERIOD: %d -> %d", g_last_period, period);
            if (period == 2) {
                g_fouls = g_stops = g_restarts = 0;
                g_late_goals = 0;
            }
            g_last_period = period;
        }
    }

    /* Read scores (offsets TBD) */
    int score_home = 0, score_away = 0;
    if (ler_mem(match_data_ptr + MGR_OFF_SCORE_HOME, &score_home, sizeof(score_home)) &&
        ler_mem(match_data_ptr + MGR_OFF_SCORE_AWAY, &score_away, sizeof(score_away))) {
        if (score_home != g_last_score_home || score_away != g_last_score_away) {
            if (score_home > g_last_score_home) {
                g_goals++;
                if (g_last_period >= 2 && g_module_added > 0) g_late_goals++;
                logf_("GOAL HOME: %d -> %d", g_last_score_home, score_home);
            }
            if (score_away > g_last_score_away) {
                g_goals++;
                if (g_last_period >= 2 && g_module_added > 0) g_late_goals++;
                logf_("GOAL AWAY: %d -> %d", g_last_score_away, score_away);
            }
            g_last_score_home = score_home;
            g_last_score_away = score_away;
        }
    }

    /* Read match time (offset TBD) */
    uint64_t match_time = 0;
    if (ler_mem(match_data_ptr + MGR_OFF_MATCH_TIME, &match_time, sizeof(match_time))) {
        g_last_match_time = match_time;
    }
}

/* ------------------------------------------------------------------------- */
/* MAIN WORKER THREAD                                                        */
/* ------------------------------------------------------------------------- */
static void *trabalho(void *arg)
{
    (void)arg;

    sceKernelMkdir("/data/jk", 0777);
    sceKernelMkdir(PASTA, 0777);
    logf_("JK_REALADDEDTIME v1.00 iniciado");

    struct proc_info pi; memset(&pi, 0, sizeof pi);
    if (sys_sdk_proc_info(&pi) != 0) { logf_("ERRO: proc_info"); return 0; }

    if (strncmp(pi.titleid, "CUSA18676", 9) != 0) {
        logf_("Nao e PES 2021 (CUSA18676), titulo=%s", pi.titleid);
        return 0;
    }

    g_base = pi.base_address;
    g_rand_seed ^= (uint32_t)g_base;
    logf_("titulo=%s base=0x%llx", pi.titleid, (unsigned long long)g_base);

    /* Wait for match to load */
    sceKernelUsleep(15000000);

    /* === FIRST BUILD: READ-ONLY MODE ===
     * Log state every 5s, dump match_data after prologues verified, verify prologues
     */
    logf_("MODO LEITURA: logando estado a cada 5s, dump match_data apos prologos, verifica prologues");

    int contador = 0;
    while (g_running) {
        sceKernelUsleep(5000000);  /* 5 seconds */

        scan_match_state();

        if (g_last_period >= 2) {
            g_module_added = calcular_acrescimo();
        }

        contador++;
        logf_("STATUS: per=%d sc=%d-%d t=%llu added_g=%d added_m=%d hud=0x%llx sb_ok=%d ref_ok=%d",
              g_last_period, g_last_score_home, g_last_score_away,
              (unsigned long long)g_last_match_time,
              g_game_announced_added, g_module_added, (unsigned long long)g_hud_ptr,
              g_scoreboard_prologue_ok, g_referee_prologue_ok);

        /* === AFTER OFFSETS CONFIRMED AND PROLOGUES VERIFIED ===
         * Uncomment ONE hook at a time:
         *
         * if (contador == 1 && g_scoreboard_prologue_ok) {
         *     if (instalar_hook(RVA_SCOREBOARD_SNAPSHOT, g_scoreboard_esperado, SCOREBOARD_PROLOGO_SIZE,
         *                       &g_scoreboard_volta, (void *)hook_scoreboard_snapshot, "Scoreboard")) {
         *         g_scoreboard_hook_installed = 1;
         *     }
         * }
         * if (contador == 2 && g_referee_prologue_ok) {
         *     if (instalar_hook(RVA_REFEREE_TIME_UP, g_referee_esperado, REFEREE_PROLOGO_SIZE,
         *                       &g_referee_volta, (void *)hook_referee_time_up, "Referee")) {
         *         g_referee_hook_installed = 1;
         *     }
         * }
         */
    }

    remover_hook(RVA_SCOREBOARD_SNAPSHOT, g_scoreboard_esperado, SCOREBOARD_PROLOGO_SIZE,
                 &g_scoreboard_hook_installed, "Scoreboard");
    remover_hook(RVA_REFEREE_TIME_UP, g_referee_esperado, REFEREE_PROLOGO_SIZE,
                 &g_referee_hook_installed, "Referee");

    if (g_log >= 0) { sceKernelClose(g_log); g_log = -1; }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* PLUGIN ENTRY POINTS                                                       */
/* ------------------------------------------------------------------------- */
int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    scePthreadCreate(&g_thr, 0, trabalho, 0, "jk_realaddedtime");
    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    g_running = 0;
    return 0;
}

int32_t attr_module_hidden module_start(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
int32_t attr_module_hidden module_stop(size_t argc, const void *args) { (void)argc; (void)args; return 0; }