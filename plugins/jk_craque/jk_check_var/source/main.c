/*
 * JK_CHECK-VAR - VAR funcional para PES 2021 (CUSA18676).
 *
 * O QUE ESTE PLUGIN FAZ DE VERDADE (sem precisar de nenhum asset novo):
 *   Hooka o unico ponto do jogo (FUN_00311c90, RVA 0x311c90) que recebe a
 *   decisao de seletor de cena de falta/cartao (resultado de FUN_00311ce0,
 *   a funcao que decide qual cartao/falta vai rolar). Depois que o jogo
 *   decide sozinho o que ia acontecer, a gente verifica se foi uma das tres
 *   familias de cartao conhecidas (sem cartao / amarelo / vermelho) e, com
 *   uma chance configuravel, TROCA a decisao original por outra da MESMA
 *   familia de variante (pra manter a cena/animacao compativel):
 *     - sem cartao -> vira cartao amarelo  (VAR flagra falta que passou batida)
 *     - amarelo    -> vira vermelho        (VAR agrava a falta)
 *     - vermelho   -> vira amarelo         (VAR anula o vermelho)
 *   Isso usa 100% de cenas/arquivos que ja existem no jogo - e o efeito real
 *   que o VAR tem numa partida de verdade (mudar a decisao do juiz).
 *
 *   A CABINE/TELA 3D de revisao (a novidade visual que o mod de PC tem) NAO
 *   esta neste pacote: ela precisa de modelo/animacao novos que nao existem
 *   no jogo (nem no PC original - o proprio mod cria isso do zero). Isso
 *   fica pra uma etapa separada de criacao de asset, se quiser seguir depois.
 *
 * Gatilho (crie o arquivo vazio por FTP em /data/jk/super/):
 *   desligar_var.txt -> desativa as trocas (so fica logando, sem mexer em nada)
 *   (apague o arquivo pra ligar de nov o)
 *
 * Log em /data/jk/super/check_var.log
 *
 * As primeiras 3 ocorrencias elegiveis em cada carregamento do plugin sao
 * SEMPRE revisadas (forcado), pra garantir que da pra ver o efeito numa
 * partida curta de teste. Depois disso passa a ser por chance (ver as
 * constantes CHANCE_* abaixo).
 *
 * ATENCAO: isso instala um hook de CODIGO (nao so leitura/escrita de dados
 * como os plugins anteriores). Se nao pegar (travar/fechar o jogo), tira o
 * jk_check_var.prx da pasta de plugins do GoldHEN e reinicia - nao mexe em
 * save nem em nenhum outro arquivo.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <orbis/libkernel.h>
#include "GoldHEN.h"
#include "Detour.h"

#define attr_public __attribute__((visibility("default")))
#define attr_module_hidden __attribute__((weak)) __attribute__((visibility("hidden")))

attr_public const char *g_pluginName = "JK_CHECK-VAR";
attr_public const char *g_pluginDesc = "JK PATCHS - VAR funcional (revisao de cartoes)";
attr_public const char *g_pluginAuth = "JK PATCHS";
attr_public uint32_t g_pluginVersion = 0x00000200;

#define PASTA "/data/jk/super"

/* RVA (offset dentro do eboot, base 0 no Ghidra) da funcao-gancho.
   E a UNICA chamadora de FUN_00311ce0 (confirmado via XREF no Ghidra). */
#define RVA_HOOK_ALVO 0x311c90ULL

/* familias de variante (indice 0,1,2) confirmadas no Ghidra dentro de
   FUN_00311ce0. Cada familia tem o mesmo indice de variante entre si,
   entao trocar dentro da mesma posicao mantem a animacao/ator compativeis. */
static const uint32_t FAMILIA_SEM_CARTAO[] = { 0x20011u, 0x20014u };
static const uint32_t FAMILIA_AMARELO[]    = { 0x2000Fu, 0x20012u, 0x20015u };
static const uint32_t FAMILIA_VERMELHO[]   = { 0x20010u, 0x20013u, 0x20016u };

#define N_SEM_CARTAO ((int)(sizeof(FAMILIA_SEM_CARTAO) / sizeof(FAMILIA_SEM_CARTAO[0])))
#define N_AMARELO    ((int)(sizeof(FAMILIA_AMARELO)    / sizeof(FAMILIA_AMARELO[0])))
#define N_VERMELHO   ((int)(sizeof(FAMILIA_VERMELHO)   / sizeof(FAMILIA_VERMELHO[0])))

#define N_FORCADAS            3   /* primeiras ocorrencias sempre revisadas, pra dar pra ver o efeito rapido */
#define CHANCE_CATAR_CARTAO   70  /* sem cartao -> amarelo */
#define CHANCE_AGRAVAR        40  /* amarelo -> vermelho */
#define CHANCE_ANULAR         35  /* vermelho -> amarelo */

typedef uint64_t (*fn_hook_alvo_t)(uint64_t fila_obj, uint64_t a, uint64_t b);

static Detour g_detour;
static uint64_t g_base;
static int g_log = -1;
static volatile uint32_t g_contador = 0;
static volatile uint32_t g_revisoes = 0;
static uint32_t g_semente = 0x9E3779B9u;

static void logf_(const char *fmt, ...)
{
    char b[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    if (g_log < 0) g_log = sceKernelOpen(PASTA "/check_var.log", 0x0001 | 0x0200 | 0x0008, 0666); /* WRONLY|CREAT|APPEND */
    if (g_log >= 0) sceKernelWrite(g_log, b, n);
}

static int existe(const char *p)
{
    OrbisKernelStat st;
    return sceKernelStat(p, &st) == 0;
}

/* xorshift32 simples, so pra sortear % sem precisar de rand() da libc */
static uint32_t prox_aleatorio(void)
{
    uint32_t x = g_semente;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_semente = x;
    return x;
}

static int sorteio_percent(int chance)
{
    return (int)(prox_aleatorio() % 100u) < chance;
}

static int indice_em(const uint32_t *lista, int n, uint32_t valor)
{
    for (int i = 0; i < n; i++) if (lista[i] == valor) return i;
    return -1;
}

/* decide se/como trocar o seletor. devolve 1 se trocou (escreve em *valor) */
static int revisar(uint32_t *valor)
{
    int idx;
    int forcado = (g_revisoes < N_FORCADAS);

    idx = indice_em(FAMILIA_SEM_CARTAO, N_SEM_CARTAO, *valor);
    if (idx >= 0) {
        if (forcado || sorteio_percent(CHANCE_CATAR_CARTAO)) {
            *valor = FAMILIA_AMARELO[idx];
            return 1;
        }
        return 0;
    }

    idx = indice_em(FAMILIA_AMARELO, N_AMARELO, *valor);
    if (idx >= 0) {
        if (forcado || sorteio_percent(CHANCE_AGRAVAR)) {
            *valor = FAMILIA_VERMELHO[idx];
            return 1;
        }
        return 0;
    }

    idx = indice_em(FAMILIA_VERMELHO, N_VERMELHO, *valor);
    if (idx >= 0) {
        if (forcado || sorteio_percent(CHANCE_ANULAR)) {
            *valor = FAMILIA_AMARELO[idx];
            return 1;
        }
        return 0;
    }

    return 0;
}

static uint64_t hook_alvo(uint64_t fila_obj, uint64_t a, uint64_t b)
{
    uint64_t ret = Detour_Stub(&g_detour, fn_hook_alvo_t, fila_obj, a, b);

    g_contador++;

    if (ret) {
        uint64_t cursor = *(volatile uint64_t *)(ret + 0x10);
        uint32_t *slot = (uint32_t *)(cursor - 4);
        uint32_t original = *slot;
        uint32_t novo = original;

        if (!existe(PASTA "/desligar_var.txt") && revisar(&novo)) {
            *slot = novo;
            g_revisoes++;
            logf_("#%u VAR: 0x%08x -> 0x%08x (revisao #%u)", g_contador, original, novo, g_revisoes);
        } else {
            logf_("#%u selector=0x%08x (sem revisao)", g_contador, original);
        }
    } else {
        logf_("#%u retorno nulo (fila_obj=0x%llx)", g_contador, (unsigned long long)fila_obj);
    }

    return ret;
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;

    sceKernelMkdir("/data/jk", 0777);
    sceKernelMkdir(PASTA, 0777);

    struct proc_info pi; memset(&pi, 0, sizeof pi);
    if (sys_sdk_proc_info(&pi) != 0) { logf_("ERRO proc_info"); return 0; }
    g_base = pi.base_address;
    g_semente ^= (uint32_t)g_base;

    logf_("JK_CHECK-VAR (VAR funcional) titulo=%s base=0x%llx hook_em=0x%llx",
          pi.titleid, (unsigned long long)g_base, (unsigned long long)(g_base + RVA_HOOK_ALVO));

    Detour_Construct(&g_detour, DetourMode_x64);
    Detour_DetourFunction(&g_detour, g_base + RVA_HOOK_ALVO, (void *)hook_alvo);

    logf_("Hook instalado. VAR ativo (crie desligar_var.txt pra desativar).");
    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    Detour_Destroy(&g_detour);
    logf_("Hook removido.");
    return 0;
}

int32_t attr_module_hidden module_start(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
int32_t attr_module_hidden module_stop(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
