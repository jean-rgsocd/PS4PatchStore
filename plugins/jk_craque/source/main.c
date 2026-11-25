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
 *
 * v3.02: o jogo parou de travar (fix da v3.01 confirmado), mas jogando uma
 * partida de teste (varios amarelos e 2 vermelhos) o check_var.log nao
 * ganhou NENHUMA linha nova alem do "Hook instalado" - ou seja, nenhuma
 * revisao aconteceu, mas tambem nao da pra saber de fora POR QUE (o log so
 * registra quando realmente troca algo, de proposito, pra nao travar como
 * antes). Essa versao so ACRESCENTA logs de diagnostico incondicionais nas
 * primeiras chamadas do hook (ver LIMITE_LOG_DIAG/g_diag_logados), pra
 * descobrir se o hook esta sendo chamado, e se sim, com que valor de
 * seletor - sem mudar nenhum comportamento de jogo. Depois de confirmado o
 * que esta acontecendo, essa parte de diagnostico sai.
 *
 * v3.03: testou a v3.02 (2 vermelhos numa partida) e ZERO linhas de DIAG
 * apareceram - nem "chamada OK" nem "ret/cursor implausivel". Ou seja,
 * hook_alvo nunca executou nenhuma vez. Isso aponta pra ANTES do hook em
 * si: o Detour_DetourFunction() pode estar falhando silenciosamente (ele
 * devolve NULL se a funcao-alvo nao existir ou se nao conseguir caber o
 * salto de 14 bytes), e o codigo antigo nunca checava esse retorno - so
 * logava "Hook instalado" sempre, mesmo se tivesse falhado. Essa versao
 * loga explicitamente se Detour_DetourFunction falhou.
 *
 * v3.04: confirmado - Detour_DetourFunction devolveu NULL em 0x711c90, ou
 * seja, FALHOU ao instalar o hook (log: "FALHOU ao instalar o hook"). Isso
 * quase certamente significa que a RVA 0x311c90 nao aponta mais pro comeco
 * de uma funcao de verdade nessa build/patch atual do jogo (pode ter mudado
 * de versao do eboot desde que isso foi mapeado no Ghidra). Essa versao so
 * ACRESCENTA um log que mostra os 16 bytes crus que estao de fato nesse
 * endereco agora (antes de tentar o hook, so leitura) - pra comparar com o
 * que o Ghidra mostra pra FUN_00311c90 no eboot ATUAL (o mesmo que esta
 * instalado no PS4 agora) e confirmar se bateu ou se mudou.
 *
 * v3.05: ACHEI A CAUSA REAL do "FALHOU ao instalar o hook". Os bytes
 * mostrados pela v3.04 em 0x711c90 sao:
 *   55                   push rbp
 *   48 89 e5             mov rbp, rsp
 *   41 56                push r14
 *   53                   push rbx
 *   c5 f8 57 c0          vxorps xmm0,xmm0,xmm0   <- instrucao AVX (VEX/0xC5)
 *   48 89 fb             mov rbx, rdi
 * Isso fecha EXATAMENTE 14 bytes, no limite certo de instrucao - a funcao
 * e o endereco sempre estiveram certos. O problema e que o desmontador
 * hde64 (usado pelo SDK do GoldHEN pra calcular quantos bytes pode
 * sobrescrever) e antigo e NAO reconhece o prefixo VEX (0xC5) das
 * instrucoes AVX - ele erra ao chegar nesse byte e devolve tamanho 0,
 * fazendo o Detour_DetourFunction desistir achando que nao cabe o salto de
 * 14 bytes, quando na verdade cabe certinho.
 *
 * A correcao: em vez de usar Detour_DetourFunction (que depende do hde64),
 * essa versao instala o hook manualmente com o tamanho de 14 bytes que eu
 * mesmo contei e confirmei acima (ver hook_manual_14() mais abaixo) -
 * usando as mesmas funcoes internas (Detour_WriteJump64) que o SDK ja usa,
 * so pulando a parte que calcula o tamanho errado.
 *
 * v3.06: testou a v3.05 e o CONSOLE INTEIRO desligou (nao so o jogo) - a
 * primeira vez que isso aconteceu numa versao que de fato chega a ESCREVER
 * em codigo do jogo (todas as anteriores so falhavam e voltavam sem mexer
 * em nada). So que depois de religar, o check_var.log NAO TINHA NENHUMA
 * linha nova - ou seja, nao da pra saber se foi esse plugin ou coincidencia
 * (o usuario confirmou que desligamentos assim ja aconteceram nesse PS4
 * mesmo sem esse ou qualquer outro plugin rodando). Pra nao ficar no escuro
 * de novo, essa versao so ACRESCENTA um log de checkpoint em CADA passo
 * arriscado de hook_manual_14 (mmap, escrita de salto, mprotect, memcpy,
 * e o passo final que de fato sobrescreve a funcao original) - se desligar
 * de novo, da pra saber exatamente em qual desses passos parou.
 *
 * v3.07: testou a v3.06 e o console desligou DE NOVO (2a vez seguida numa
 * versao que escreve codigo). Comparei com o jk_craque - plugin do MESMO
 * projeto que ja faz hook de codigo (prologo de 17 bytes, MAIOR que o
 * nosso) e NUNCA foi reportado travando o console - e ele faz 3 coisas
 * diferentes da nossa v3.05/v3.06:
 *   1) escreve os bytes via sys_sdk_proc_rw (syscall do SDK pra escrever em
 *      memoria de processo), nao memcpy/mprotect direto;
 *   2) espera 3s (sceKernelUsleep) antes de mexer no executavel;
 *   3) os bytes originais da funcao ficam escritos a mao como assembly fixo
 *      no proprio plugin (nao copiados dinamicamente pra memoria alocada
 *      em runtime via sceKernelMmap).
 * Essa versao reescreve a instalacao do hook inteira seguindo EXATAMENTE
 * esse padrao ja comprovado (ver instalar()/gravar_eboot()/
 * jk_hookalvo_orig() mais abaixo). O Detour.h/API do GoldHEN SDK foi
 * removido por completo - no usa mais Detour_Construct/Destroy/Stub.
 *
 * v3.07 TESTADA: instalou com sucesso ("GANCHO ok") em 2 boots seguidos, SEM
 * nenhum desligamento de console - o problema da v3.05/v3.06 estava mesmo
 * na instalacao, nao no alvo. So que, jogando de verdade (ate um cartao
 * VERMELHO), o check_var.log continuou SEM NENHUMA linha de DIAG - ou seja,
 * hook_alvo() nunca chegou a executar nenhuma vez, mesmo com o hook
 * instalado corretamente.
 *
 * v4.00: ACHADO O MOTIVO. Baixei o eboot atual (eboot_CUSA18676.bin, dump
 * de memoria RVA==offset-no-arquivo, base 0x400000) e procurei TODAS as
 * formas de chamada pra FUN_00311c90 (call direta E8, LEA RIP-relative
 * calculando o endereco, ponteiro absoluto de 8 bytes em qualquer lugar do
 * arquivo): ZERO ocorrencias em 74MB de codigo/dados. FUN_00311c90
 * simplesmente nao e chamada por ninguem nesse eboot - a premissa antiga
 * ("unico chamador de FUN_00311ce0") estava desatualizada/errada.
 *
 * Procurando por quem chama FUN_00311ce0 (a funcao que DE FATO decide o
 * cartao) achei DOIS chamadores: RVA 0x311cb1 (dentro da propria
 * FUN_00311c90, o hook antigo - so repassa o resultado adiante) e RVA
 * 0x3119d2, dentro de uma funcao BEM maior e completamente diferente, que e
 * quase certamente o caminho usado de verdade numa partida.
 *
 * Em vez de escolher qual das duas chamadoras hookar (arriscando errar de
 * novo se o jogo usar um terceiro caminho futuramente), essa versao hooka
 * FUN_00311ce0 DIRETAMENTE - a funcao que as duas chamadoras efetivamente
 * usam. Conferi os dois pontos de chamada no disassembly: ambos, logo
 * depois do "call FUN_00311ce0", so consultam o valor devolvido em EAX (nao
 * leem nenhum outro dado da chamada) - ou seja, basta trocar o VALOR DE
 * RETORNO da funcao pra mudar a decisao, sem precisar achar "cursor"/
 * ponteiro de objeto nenhum como a v3.x fazia. Isso cobre os dois
 * chamadores conhecidos automaticamente, e e mais simples e mais robusto.
 *
 * O prologo de FUN_00311ce0 e maior (20 bytes ate a proxima instrucao
 * completa: push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12;
 * push rbx; sub rsp,0x668) - sem nenhuma instrucao AVX/VEX dessa vez, mas
 * mantive o mesmo padrao comprovado do jk_craque/v3.07 (sys_sdk_proc_rw,
 * delay de 3s, assembly fixo) por seguranca.
 *
 * v4.03: trabalho de pesquisa pra tentar abrir de vez a SALA 3D do VAR (nao
 * so a troca de cartao, que ja funciona desde a v4.00). Li o FoulScenes.lua
 * completo do seu mod de PC. Descobri duas coisas importantes:
 *
 * 1) O mod de PC so mexe na sala do VAR em situacoes de PENALTI, usando uma
 *    familia de seletor DIFERENTE da nossa (0x0002000F a 0x00020014, uma
 *    pra cada variante de cartao NO PENALTI: Y=amarelo, R=vermelho,
 *    N=sem cartao, W=aviso, R_ALT/N_ALT=variantes). Isso bate com um valor
 *    que ja vimos SOZINHO num log antigo seu (log do teste de
 *    desligar/ligar sala): "#4 DIAG: chamada OK, original=0x00020011" -
 *    0x20011 e EXATAMENTE o "FOUL_PK_CARD_N" (penalti sem cartao) da tabela
 *    do PC. Ou seja, e bem provavel que o PS4 use OS MESMOS numeros do PC
 *    pra penalti (diferente dos numeros de falta comum, que descobrimos na
 *    v4.01 que eram outros). Essa versao passa a reconhecer e logar esses 6
 *    valores de penalti separadamente (ver FAMILIA_PENALTI abaixo), pra
 *    confirmar com certeza na proxima partida com penalti.
 *
 * 2) ACHEI O MOTIVO PROVAVEL de 0x00050006 (sozinho) ter mostrado a cena de
 *    troca de jogador em vez da sala do VAR no seu teste: lendo o Lua, o
 *    PROPRIO mod de PC SO troca o seletor pra 0x00050006 DEPOIS de escrever
 *    um "perfil" de 26 bytes numa estrutura de memoria separada (endereco =
 *    [RAX]+[R15] no ponto que ELE hooka, que e diferente do nosso - o PC
 *    hooka onde o jogo CONSOME o seletor, nao onde ele e GERADO). Se esse
 *    perfil nao e escrito, o PROPRIO codigo do PC (nao so o jogo) cai de
 *    volta pra cena de troca de jogador como "plano B" - exatamente o que
 *    vimos no PS4. Ou seja, muito provavelmente SO TROCAR O NUMERO NAO E
 *    SUFICIENTE, nem no PC nem no PS4 - falta a outra metade do mecanismo
 *    (escrever esse perfil em memoria).
 *
 *    Tentei achar, so analisando o eboot (sem depurador ao vivo - nao tenho
 *    acesso a isso neste ambiente), a funcao que faz esse trabalho no PS4.
 *    Rastreei os dois chamadores conhecidos de FUN_00311ce0 pra frente: os
 *    dois, logo depois da chamada, chamam uma funcao comum (offset 0x714660)
 *    passando o resultado - mas essa funcao e so uma rotina generica de
 *    crescimento de std::vector (reutilizada em centenas de lugares do
 *    jogo, sem nada especifico de VAR) - um beco sem saida pra analise
 *    estatica sozinha.
 *
 *    Por isso essa versao troca o experimento de "um numero fixo so" (que
 *    ja provamos que NAO abre a sala sozinho) por uma BUSCA: uma lista de
 *    varios candidatos da mesma familia (0x00050000 a 0x0005000A), testando
 *    UM DIFERENTE a cada falta elegivel enquanto o modo de busca estiver
 *    ligado, com log bem claro de qual candidato foi tentado - pra voce
 *    testar em jogo e me dizer exatamente o que cada um mostrou (igual a
 *    gente fez pra descobrir os valores de cartao na v4.01). Sem um
 *    depurador ao vivo no PS4, isso e o jeito mais rapido e confiavel de
 *    achar o numero certo (se for so numero) ou confirmar que falta mesmo
 *    o tal "perfil" de memoria (se NENHUM candidato abrir a sala de
 *    verdade).
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

attr_public const char *g_pluginName = "JK_CHECK-VAR";
attr_public const char *g_pluginDesc = "JK PATCHS - VAR funcional (revisao de cartoes)";
attr_public const char *g_pluginAuth = "JK PATCHS";
attr_public uint32_t g_pluginVersion = 0x00000404;

#define PASTA "/data/jk/super"

/* RVA (offset dentro do eboot, base 0 no Ghidra) de FUN_00311ce0 - a funcao
   que DE FATO decide o cartao/falta (nao mais FUN_00311c90, que nenhum
   codigo do eboot chama de verdade - ver nota v4.00 mais acima). Confirmado
   lendo o eboot_CUSA18676.bin real: os dois chamadores conhecidos (RVA
   0x311cb1 e 0x3119d2) so consultam o retorno (EAX) dessa funcao. */
#define RVA_HOOK_ALVO 0x311ce0ULL

/* v4.01: valores REAIS, confirmados jogando com o DIAG da v4.00 ligado (nao
   mais os antigos 0x2000F-0x20016, que eram de uma analise de Ghidra
   desatualizada e NUNCA bateram com nada em jogo de verdade):
     0x20001, 0x20006 -> amarelo (confirmado 4x: 1o cartao deu 0x20006, os
       outros 3 deram 0x20001 - sao so variantes/cameras diferentes, mesma
       gravidade)
     0x20004 -> a cena de "segundo amarelo vira vermelho" (expulsao)
   Ainda NAO vimos nenhum valor de "falta sem cartao nenhum" passar por essa
   funcao - pelo visto ela so e chamada quando ja vai sair cartao (uma falta
   comum talvez nem chegue a chamar FUN_00311ce0). Por isso o grupo
   FAMILIA_SEM_CARTAO foi removido por enquanto; se aparecer um valor novo
   fora dessas duas familias, e provavelmente isso - me avisa.
   Como as familias tem tamanhos diferentes agora (2 amarelos pra 1
   vermelho), a troca passou a sortear um valor aleatorio DENTRO da familia
   de destino, em vez de preservar o mesmo indice (que so fazia sentido
   quando todas tinham o mesmo tamanho). */
static const uint32_t FAMILIA_AMARELO[]    = { 0x20001u, 0x20006u };
static const uint32_t FAMILIA_VERMELHO[]   = { 0x20004u };

#define N_AMARELO    ((int)(sizeof(FAMILIA_AMARELO)    / sizeof(FAMILIA_AMARELO[0])))
#define N_VERMELHO   ((int)(sizeof(FAMILIA_VERMELHO)   / sizeof(FAMILIA_VERMELHO[0])))

#define N_FORCADAS            3   /* primeiras ocorrencias elegiveis sempre revisadas, pra dar pra ver o efeito rapido */
#define CHANCE_AGRAVAR        40  /* amarelo -> vermelho */
#define CHANCE_ANULAR         35  /* vermelho -> amarelo */

/* v4.03: valores de PENALTI vindos do FoulScenes.lua do seu mod de PC
   (tabela PK_ROUTES) - DIFERENTES da familia de falta comum acima. Ainda
   nao confirmados 100% no PS4 (so vimos 0x20011 aparecer uma vez, sem eu
   saber na hora que era penalti), mas como ja bateu exatamente com o nome
   certo (FOUL_PK_CARD_N = penalti sem cartao), essa versao passa a
   reconhecer e logar os 6 de forma BEM visivel sempre que aparecerem - isso
   so serve pra CONFIRMAR em jogo, nao muda nenhuma decisao sozinho ainda. */
static const uint32_t FAMILIA_PENALTI[] = {
    0x2000Fu, /* PK_CARD_Y      - penalti, amarelo */
    0x20010u, /* PK_CARD_R      - penalti, vermelho */
    0x20011u, /* PK_CARD_N      - penalti, sem cartao (ja visto 1x no log) */
    0x20012u, /* PK_CARD_W      - penalti, aviso/sem cartao (variante) */
    0x20013u, /* PK_CARD_R_ALT  - penalti, vermelho (variante) */
    0x20014u, /* PK_CARD_N_ALT  - penalti, sem cartao (variante) */
};
#define N_PENALTI ((int)(sizeof(FAMILIA_PENALTI) / sizeof(FAMILIA_PENALTI[0])))

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
 * v4.03: JA CONFIRMAMOS que 0x00050006 sozinho NAO abre a sala (mostrou a
 * cena de troca de jogador, igual ao "plano B" que o proprio Lua do PC usa
 * quando falta o perfil de memoria - ver nota grande la em cima). Por isso
 * trocamos de "um numero fixo" pra uma BUSCA numa lista de candidatos da
 * mesma familia (CANDIDATOS_SALA_VAR abaixo) - cada vez que essa ocorrencia
 * acontece com o modo de busca ligado, tenta o PROXIMO candidato da lista
 * (nunca repete o mesmo 0x50006 que ja sabemos que nao funciona sozinho).
 * Precisa testar em jogo, UM POR VEZ, e me dizer exatamente o que cada
 * tentativa mostrou na tela (igual fizemos pra descobrir os valores reais
 * de cartao). Se NENHUM candidato abrir a sala de verdade, isso confirma
 * que falta mesmo a escrita do tal "perfil" de memoria, e aí o proximo
 * passo vira tentar achar esse segundo ponto certo no PS4 (precisa de mais
 * investigacao, possivelmente com logs de registradores que esse plugin
 * ainda nao tira). Se travar numa tentativa: apaga o ligar_sala_var.txt e
 * volta pro modo seguro (que e o padrao).
 *
 * v4.04: TESTADO - 0x00050000 (1o candidato da lista) trava a ANIMACAO do
 * jogo (juiz/jogadores congelados no lugar, bola parada no ar) enquanto o
 * relogio da partida continua passando normal - ou seja, nao derruba o
 * jogo/console, mas fica preso num carregamento de cena que nunca termina,
 * e so da pra sair fechando o jogo na mao. Removido da lista (NAO tenta
 * mais esse valor). Os candidatos seguintes ja pulam direto pra 0x50002. */
static const uint32_t CANDIDATOS_SALA_VAR[] = {
    0x50002u, 0x50003u, 0x50004u, 0x50005u,
    0x50007u, 0x50008u, 0x50009u, 0x5000Au, 0x5000Bu,
};
#define N_CANDIDATOS_SALA_VAR ((int)(sizeof(CANDIDATOS_SALA_VAR) / sizeof(CANDIDATOS_SALA_VAR[0])))
#define LIMITE_TENTATIVAS_SALA    N_CANDIDATOS_SALA_VAR
#define CHANCE_SALA_VAR           65   /* mesma chance que o mod de PC usa (var_event_probability) */

static uint64_t g_base;
static int g_log = -1;
static volatile uint32_t g_contador = 0;
static volatile uint32_t g_revisoes = 0;
static uint32_t g_semente = 0x9E3779B9u;
static volatile int g_var_ligado = 1;
static volatile int g_sala_var_habilitada = 0;   /* rechecado a cada INTERVALO_RECHECAGEM */
static volatile uint32_t g_tentativas_sala = 0;
static OrbisPthread g_thr;

/* v4.01: confirmado jogando que FUN_00311ce0 (nosso alvo desde a v4.00) so
   e chamada umas poucas vezes por partida (uma por falta/cartao, nao por
   frame) - bem diferente da estimativa antiga de "centenas de vezes" que
   valia pro hook errado da v3.x. Por isso agora confere desligar_var.txt e
   ligar_sala_var.txt em TODA chamada (sceKernelStat e barato nessa
   frequencia) - antes so reconferia a cada 2000 chamadas, o que na pratica
   nunca acontecia numa partida so, entao apagar/criar esses arquivos no
   meio do jogo nao tinha efeito ate reiniciar o jogo. */
#define INTERVALO_RECHECAGEM 1u

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

/* decide se/como trocar o seletor. devolve 1 se trocou (escreve em *valor).
   v4.01: como as familias nao tem mais o mesmo tamanho entre si, a troca
   sorteia um indice aleatorio DENTRO da familia de destino (nao da mais pra
   preservar "mesma posicao = mesmo ator/cena" como antes). */
static int revisar(uint32_t *valor)
{
    int idx;
    int forcado = (g_revisoes < N_FORCADAS);

    idx = indice_em(FAMILIA_AMARELO, N_AMARELO, *valor);
    if (idx >= 0) {
        if (forcado || sorteio_percent(CHANCE_AGRAVAR)) {
            *valor = FAMILIA_VERMELHO[prox_aleatorio() % N_VERMELHO];
            return 1;
        }
        return 0;
    }

    idx = indice_em(FAMILIA_VERMELHO, N_VERMELHO, *valor);
    if (idx >= 0) {
        if (forcado || sorteio_percent(CHANCE_ANULAR)) {
            *valor = FAMILIA_AMARELO[prox_aleatorio() % N_AMARELO];
            return 1;
        }
        return 0;
    }

    return 0;
}

/* DIAG TEMPORARIO (v3.02): loga INCONDICIONALMENTE as primeiras chamadas do
   hook, pra descobrir se ele esta sendo chamado e que valores esta vendo -
   mesmo quando nada bate com as familias conhecidas ou quando as checagens
   de seguranca abaixo decidem nao mexer em nada. Isso NAO afeta o
   comportamento normal, so adiciona visibilidade. Remover depois de
   confirmado o fluxo. */
#define LIMITE_LOG_DIAG 25u
static volatile uint32_t g_diag_logados = 0;

static uint32_t jk_hookalvo_orig(uint64_t a, uint64_t b);

/* v4.00: hook direto em FUN_00311ce0 - a propria funcao que decide o
   cartao/falta. O valor original (o "veredito" do jogo) vem pronto no
   retorno da funcao original (EAX) - so precisamos trocar esse valor antes
   de devolver pra quem chamou. Nao precisa mais achar cursor/ponteiro de
   objeto nenhum (isso so fazia sentido quando a gente hookava o
   CHAMADOR). */
static uint32_t hook_alvo(uint64_t a, uint64_t b)
{
    uint32_t original = jk_hookalvo_orig(a, b);
    uint32_t n = ++g_contador;

    if (g_diag_logados < LIMITE_LOG_DIAG) {
        g_diag_logados++;
        logf_("#%u DIAG: chamada OK, original=0x%08x", n, original);
    }

    /* v4.03: loga BEM visivel sempre que um valor de penalti (familia do mod
       de PC) aparecer - sem limite de quantidade, sem mexer em nada ainda,
       so pra confirmar com certeza se o PS4 usa os mesmos numeros do PC pra
       penalti (ver nota grande no topo do arquivo). */
    {
        int idx_pk = indice_em(FAMILIA_PENALTI, N_PENALTI, original);
        if (idx_pk >= 0) {
            static const char *NOMES_PENALTI[] = {
                "PK_CARD_Y (penalti amarelo)", "PK_CARD_R (penalti vermelho)",
                "PK_CARD_N (penalti sem cartao)", "PK_CARD_W (penalti aviso)",
                "PK_CARD_R_ALT (penalti vermelho var.)", "PK_CARD_N_ALT (penalti sem cartao var.)",
            };
            logf_("#%u >>> PENALTI DETECTADO: 0x%08x = %s <<<", n, original, NOMES_PENALTI[idx_pk]);
        }
    }

    /* v4.01: confere em toda chamada agora (ver nota em INTERVALO_RECHECAGEM
       mais acima - essa funcao e rara, nao custa nada). */
    if ((n % INTERVALO_RECHECAGEM) == 0) {
        g_var_ligado = !existe(PASTA "/desligar_var.txt");
        g_sala_var_habilitada = existe(PASTA "/ligar_sala_var.txt");
    }

    if (!g_var_ligado) {
        return original;
    }

    /* modo experimental (desligado por padrao - ver comentario la em cima) */
    if (g_sala_var_habilitada && g_tentativas_sala < LIMITE_TENTATIVAS_SALA) {
        int pertence_a_alguma_familia =
            (indice_em(FAMILIA_AMARELO, N_AMARELO, original) >= 0) ||
            (indice_em(FAMILIA_VERMELHO, N_VERMELHO, original) >= 0);

        if (pertence_a_alguma_familia && sorteio_percent(CHANCE_SALA_VAR)) {
            uint32_t candidato = CANDIDATOS_SALA_VAR[g_tentativas_sala];
            g_tentativas_sala++;
            logf_("#%u BUSCA sala do VAR: 0x%08x -> 0x%08x (candidato %u/%u - anote o que aparece na tela!)",
                  n, original, candidato, g_tentativas_sala, (unsigned)LIMITE_TENTATIVAS_SALA);
            return candidato; /* nao aplica tambem a revisao normal nessa mesma ocorrencia */
        }
    }

    uint32_t novo = original;
    if (revisar(&novo)) {
        uint32_t r = ++g_revisoes;
        /* loga em disco so quando REALMENTE muda algo, e so as primeiras
           vezes - escrever arquivo em toda chamada do hook e a causa mais
           provavel de travar/derrubar o jogo (I/O sincrono num ponto
           chamado com muita frequencia). */
        if (r <= LIMITE_LOG_REVISOES) {
            logf_("#%u VAR: 0x%08x -> 0x%08x (revisao #%u)", n, original, novo, r);
        }
        return novo;
    }

    return original;
}

/* ------------------------------------------------------------------------
 * v3.07: instalacao do hook REFEITA seguindo exatamente o padrao do
 * jk_craque (plugin JA COMPROVADO nesse mesmo projeto, nunca reportado
 * travando o console - inclusive com um prologo MAIOR que o nosso, 17
 * bytes). Trocas em relacao a v3.05/v3.06 (que travaram o console 2x):
 *   1) escreve os bytes com sys_sdk_proc_rw (syscall do SDK feita pra
 *      escrever em memoria de processo), em vez de memcpy/mprotect direto;
 *   2) espera 3s (sceKernelUsleep) antes de mexer no executavel, deixando
 *      o jogo terminar de subir;
 *   3) em vez de copiar os bytes originais dinamicamente pra um "stub"
 *      alocado em runtime (sceKernelMmap), os bytes originais (ja
 *      confirmados byte a byte - ver nota v3.05) ficam escritos a mao,
 *      fixos, como assembly dentro do proprio plugin, com um jmp de volta
 *      pro resto da funcao original (FunctionPtr+14). Isso elimina
 *      qualquer dependencia de alocar/copiar memoria executavel em tempo
 *      de execucao.
 *   4) confere os bytes atuais contra o esperado ANTES de escrever - se
 *      nao bater (outra versao/patch do jogo), desiste e nao mexe em nada
 *      (igual ao jk_craque). */
/* prologo de FUN_00311ce0 (20 bytes, confirmado byte a byte lendo o eboot
   real): push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12;
   push rbx; sub rsp,0x668. So a gente SO ESCREVE um jmp de 14 bytes no
   inicio (ff 25 00000000 + 8 bytes de endereco) - sobra espaco de verdade,
   os ultimos 6 bytes do array abaixo so servem pra conferir que bate certo
   com o eboot antes de mexer em qualquer coisa. */
#define TAM_PROLOGO 20
static const uint8_t g_esperado[TAM_PROLOGO] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
    0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x68, 0x06, 0x00, 0x00
};
static int g_gancho = 0;
__attribute__((visibility("hidden"))) uint64_t g_hookalvo_volta;

static int gravar_eboot(uint64_t dest, const void *src, uint64_t n)
{
    struct proc_rw rw; memset(&rw, 0, sizeof rw);
    rw.address = dest; rw.data = (void *)src; rw.length = n; rw.write_flags = 1;
    if (sys_sdk_proc_rw(&rw) != 0) return 0;
    return memcmp((const void *)dest, src, n) == 0;
}

static void instalar(void)
{
    uint64_t f = g_base + RVA_HOOK_ALVO;
    const volatile uint8_t *bb = (const volatile uint8_t *)f;
    for (int i = 0; i < TAM_PROLOGO; i++) if (bb[i] != g_esperado[i]) {
        logf_("GANCHO recusado: bytes diferentes em 0x%llx (%02x %02x %02x %02x ...) - outra versao do jogo? VAR NAO esta ativo.",
              (unsigned long long)RVA_HOOK_ALVO, bb[0], bb[1], bb[2], bb[3]);
        return;
    }
    g_hookalvo_volta = f + TAM_PROLOGO;
    uint8_t salto[14];
    uint64_t alvo = (uint64_t)(void *)hook_alvo;
    salto[0] = 0xff; salto[1] = 0x25; salto[2] = salto[3] = salto[4] = salto[5] = 0;
    memcpy(salto + 6, &alvo, 8);
    if (!gravar_eboot(f, salto, sizeof salto)) {
        gravar_eboot(f, g_esperado, TAM_PROLOGO);
        logf_("GANCHO recusado: falha ao gravar (sys_sdk_proc_rw). VAR NAO esta ativo.");
        return;
    }
    g_gancho = 1;
    logf_("GANCHO ok eboot+0x%llx. VAR ativo (crie desligar_var.txt pra desativar).", (unsigned long long)RVA_HOOK_ALVO);
}

static void *trabalho(void *arg)
{
    (void)arg;

    sceKernelMkdir("/data/jk", 0777);
    sceKernelMkdir(PASTA, 0777);
    logf_("trabalho(): comecou, pastas OK");

    struct proc_info pi; memset(&pi, 0, sizeof pi);
    if (sys_sdk_proc_info(&pi) != 0) { logf_("ERRO proc_info"); return 0; }
    g_base = pi.base_address;
    g_semente ^= (uint32_t)g_base;

    logf_("JK_CHECK-VAR (VAR funcional) titulo=%s base=0x%llx hook_em=0x%llx",
          pi.titleid, (unsigned long long)g_base, (unsigned long long)(g_base + RVA_HOOK_ALVO));

    sceKernelUsleep(3000000); /* v3.07: deixa o jogo subir antes de gravar no executavel (igual ao jk_craque) */
    instalar();
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
    if (g_gancho) { gravar_eboot(g_base + RVA_HOOK_ALVO, g_esperado, TAM_PROLOGO); g_gancho = 0; }
    logf_("Hook removido.");
    return 0;
}

int32_t attr_module_hidden module_start(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
int32_t attr_module_hidden module_stop(size_t argc, const void *args) { (void)argc; (void)args; return 0; }

/* comeco original de FUN_00311ce0 (20 bytes, confirmados byte a byte lendo
   o eboot_CUSA18676.bin real - ver nota v4.00) + salto de volta pra
   eboot+RVA_HOOK_ALVO+20. Igual ao jk_arq_orig do jk_craque: em vez de
   copiar os bytes originais dinamicamente em tempo de execucao, eles ficam
   fixos aqui, escritos a mao, compilados junto com o plugin - sem overhead
   de alocar memoria executavel. */
__attribute__((naked, noinline, used)) static uint32_t jk_hookalvo_orig(uint64_t a, uint64_t b)
{
    __asm__ volatile(
        ".byte 0x55\n"                               /* push rbp */
        ".byte 0x48,0x89,0xe5\n"                     /* mov rbp,rsp */
        ".byte 0x41,0x57\n"                          /* push r15 */
        ".byte 0x41,0x56\n"                          /* push r14 */
        ".byte 0x41,0x55\n"                          /* push r13 */
        ".byte 0x41,0x54\n"                          /* push r12 */
        ".byte 0x53\n"                                /* push rbx */
        ".byte 0x48,0x81,0xec,0x68,0x06,0x00,0x00\n" /* sub rsp,0x668 */
        "jmp *g_hookalvo_volta(%rip)\n");
}
