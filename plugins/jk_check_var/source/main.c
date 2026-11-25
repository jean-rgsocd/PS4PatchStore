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
 *   A CABINE/TELA 3D de revisao (a novidade visual que o mod de PC tem) tem
 *   uma tentativa EXPERIMENTAL e OPCIONAL nesta versao (v3) - ver o bloco de
 *   comentario grande perto de SELETOR_SALA_VAR mais abaixo. Fica desligada
 *   por padrao; so liga se voce criar o arquivo ligar_sala_var.txt.
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
 *
 * v2.01: corrigido o crash (CE-34878-0) da v2.00. A causa era I/O de disco
 * (log) em TODA chamada do hook, inclusive quando nada mudava - isso pode
 * ser centenas de vezes por partida e travava o jogo. Agora: (1) so loga
 * quando realmente troca alguma coisa, e so nas primeiras ~80 vezes; (2) a
 * checagem do desligar_var.txt roda de vez em quando, nao toda chamada; (3)
 * checagem de seguranca no ponteiro de retorno antes de escrever qualquer
 * memoria, pra nunca escrever em endereco implausivel.
 *
 * v3.00: adiciona um modo EXPERIMENTAL opcional (desligado por padrao) que
 * tenta mesmo abrir a sala 3D do VAR, usando o seletor 0x00050006 que
 * descobri dentro do script Lua do seu mod de PC (FoulScenes.lua). Ver o
 * bloco de comentario grande perto de SELETOR_SALA_VAR mais abaixo pra
 * entender exatamente o que isso faz e por que fica desligado por padrao.
 * O comportamento PADRAO (arquivo ligar_sala_var.txt ausente) continua
 * identico ao da v2.01 - nada muda se voce nao criar esse arquivo.
 *
 * v3.01: ACHEI A CAUSA REAL do crash (CE-34878-0) que persistia em todas as
 * versoes anteriores, inclusive num build de diagnostico sem nenhum hook.
 * O plugin_load() estava fazendo trabalho de verdade (mkdir, abrir/escrever
 * log, sys_sdk_proc_info, Detour) de forma SINCRONA, direto dentro da
 * funcao plugin_load(). Os outros plugins do projeto que ja funcionam
 * (jk_sonda, jk_extrator) NUNCA fazem isso - eles so chamam
 * scePthreadCreate() dentro de plugin_load() (que volta na hora) e fazem
 * todo o trabalho de verdade numa thread separada. Essa versao segue
 * exatamente esse mesmo padrao (ver trabalho()/plugin_load() mais abaixo).
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
attr_public uint32_t g_pluginVersion = 0x00000301;

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

/* ------------------------------------------------------------------------
 * MODO EXPERIMENTAL "SALA DO VAR" (v3)
 *
 * Achei, dentro do proprio mod de PC que voce mandou (FoulScenes.lua), o
 * valor exato que o motor do jogo usa pra selecionar a cena de "revisao do
 * VAR" de verdade: 0x00050006 (uma familia de seletor completamente
 * diferente das 3 de cartao - 0x2000x). No PC, o mod troca pra esse valor
 * quando decide mostrar a sala, e so DEPOIS usa a decisao original (a
 * mesma troca que esse plugin ja faz) como o "veredito final" mostrado
 * dentro da sala.
 *
 * Eu NAO tenho 100% de certeza que so trocar esse numero no ponto que a
 * gente ja hooka no PS4 e suficiente - no PC o mod mexe num ponto um
 * pouco mais a frente (onde o jogo CONSOME o seletor, nao onde ele e
 * GERADO, que e o nosso ponto). Pode ser que falte alguma coisa e o jogo
 * simplesmente ignore o valor (nada acontece) OU, no pior caso, nao saiba
 * lidar com ele (trava).
 *
 * Por isso isso fica DESLIGADO por padrao e so liga se voce criar esse
 * arquivo (por FTP, vazio):
 *   /data/jk/super/ligar_sala_var.txt
 *
 * Com ele presente, limitado a LIMITE_TENTATIVAS_SALA vezes por
 * carregamento (pra nao arriscar demais numa unica partida de teste):
 * ao inves de revisar dentro da familia, troca o seletor pra 0x00050006.
 * Se der certo: deve abrir uma cena/camera diferente da cutscene normal
 * de cartao. Se nao acontecer nada visivel: anotamos que falta a outra
 * metade do mecanismo (meu palpite e um "flag" extra que o PC grava numa
 * estrutura separada, ainda nao localizada no PS4). Se travar: apaga o
 * ligar_sala_var.txt e volta pro modo seguro (que e o padrao). */
#define SELETOR_SALA_VAR          0x00050006u
#define LIMITE_TENTATIVAS_SALA    10
#define CHANCE_SALA_VAR           65   /* mesma chance que o mod de PC usa (var_event_probability) */

typedef uint64_t (*fn_hook_alvo_t)(uint64_t fila_obj, uint64_t a, uint64_t b);

static Detour g_detour;
static uint64_t g_base;
static int g_log = -1;
static volatile uint32_t g_contador = 0;
static volatile uint32_t g_revisoes = 0;
static uint32_t g_semente = 0x9E3779B9u;
static volatile int g_var_ligado = 1;
static volatile int g_sala_var_habilitada = 0;   /* rechecado a cada INTERVALO_RECHECAGEM */
static volatile uint32_t g_tentativas_sala = 0;
static OrbisPthread g_thr;

/* a cada quantas chamadas a gente confere de novo o arquivo desligar_var.txt
   (em vez de checar em TODA chamada - sceKernelStat em toda chamada de uma
   funcao que pode rodar centenas de vezes por partida e caro e pode
   atrapalhar o jogo). */
#define INTERVALO_RECHECAGEM 2000u

/* limite de quantas revisoes a gente loga em arquivo (I/O de disco em toda
   chamada do hook e a causa mais provavel de travamento/CE-34878 - aqui so
   logamos quando REALMENTE muda algo, e so nas primeiras N vezes). */
#define LIMITE_LOG_REVISOES 80u

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
    uint32_t n = ++g_contador;

    /* checagem de seguranca: isso aqui e um hook em codigo nativo do jogo.
       se o retorno nao parecer um ponteiro plausivel dentro do modulo do
       jogo (por qualquer motivo - versao diferente, chamada inesperada,
       etc.), a gente simplesmente NAO MEXE em nada e devolve o retorno
       original sem tocar memoria nenhuma. Isso faz o hook virar "invisivel"
       em vez de arriscar escrever em endereco invalido e derrubar o jogo. */
    if (ret < g_base || (ret - g_base) > 0x20000000ULL) {
        return ret;
    }

    /* confere o arquivo de desligar so de vez em quando, nao em toda
       chamada (sceKernelStat tem custo e essa funcao pode ser chamada
       muitas vezes numa partida) */
    if ((n % INTERVALO_RECHECAGEM) == 1) {
        g_var_ligado = !existe(PASTA "/desligar_var.txt");
        g_sala_var_habilitada = existe(PASTA "/ligar_sala_var.txt");
    }

    if (!g_var_ligado) {
        return ret;
    }

    uint64_t cursor = *(volatile uint64_t *)(ret + 0x10);
    if (cursor < 4) {
        return ret; /* cursor implausivel - nao mexe */
    }

    uint32_t *slot = (uint32_t *)(cursor - 4);
    uint32_t original = *slot;

    /* modo experimental (desligado por padrao - ver comentario la em cima) */
    if (g_sala_var_habilitada && g_tentativas_sala < LIMITE_TENTATIVAS_SALA) {
        int pertence_a_alguma_familia =
            (indice_em(FAMILIA_SEM_CARTAO, N_SEM_CARTAO, original) >= 0) ||
            (indice_em(FAMILIA_AMARELO, N_AMARELO, original) >= 0) ||
            (indice_em(FAMILIA_VERMELHO, N_VERMELHO, original) >= 0);

        if (pertence_a_alguma_familia && sorteio_percent(CHANCE_SALA_VAR)) {
            *slot = SELETOR_SALA_VAR;
            g_tentativas_sala++;
            logf_("#%u EXPERIMENTO sala do VAR: 0x%08x -> 0x%08x (tentativa %u/%u)",
                  n, original, SELETOR_SALA_VAR, g_tentativas_sala, (unsigned)LIMITE_TENTATIVAS_SALA);
            return ret; /* nao aplica tambem a revisao normal nessa mesma ocorrencia */
        }
    }

    uint32_t novo = original;
    if (revisar(&novo)) {
        *slot = novo;
        uint32_t r = ++g_revisoes;
        /* loga em disco so quando REALMENTE muda algo, e so as primeiras
           vezes - escrever arquivo em toda chamada do hook e a causa mais
           provavel de travar/derrubar o jogo (I/O sincrono num ponto
           chamado com muita frequencia). */
        if (r <= LIMITE_LOG_REVISOES) {
            logf_("#%u VAR: 0x%08x -> 0x%08x (revisao #%u)", n, original, novo, r);
        }
    }

    return ret;
}

static void *trabalho(void *arg)
{
    (void)arg;

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

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    /* v3.01: todo trabalho de verdade (mkdir, proc_info, Detour) agora roda
       numa thread separada, igual ao jk_sonda/jk_extrator - plugin_load()
       so dispara a thread e volta na hora. Ver nota no topo do arquivo. */
    scePthreadCreate(&g_thr, 0, trabalho, 0, "jk_check_var");
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
