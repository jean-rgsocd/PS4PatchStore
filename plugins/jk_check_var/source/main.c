/*
 * JK_CHECK_VAR_DIAG - versao MINIMA so pra diagnostico.
 *
 * NAO instala nenhum hook. So confere se o plugin carrega e escreve no
 * log. Serve pra descobrir se o crash do jk_check_var acontece:
 *   (a) no CARREGAMENTO do .prx em si (esse diagnostico tambem travaria), ou
 *   (b) dentro do Detour_Construct/Detour_DetourFunction (esse diagnostico
 *       NAO trava, e o log aparece normalmente).
 *
 * Teste isso sozinho (pode deixar os outros plugins como estavam, so troca
 * o jk_check_var.prx por esse aqui temporariamente, mesmo nome de arquivo
 * jk_check_var.prx pra nao precisar mexer no plugins.ini).
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

attr_public const char *g_pluginName = "JK_CHECK_VAR_DIAG";
attr_public const char *g_pluginDesc = "JK PATCHS - diagnostico (sem hook)";
attr_public const char *g_pluginAuth = "JK PATCHS";
attr_public uint32_t g_pluginVersion = 0x00000001;

#define PASTA "/data/jk/super"

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
    if (g_log < 0) g_log = sceKernelOpen(PASTA "/check_var_diag.log", 0x0001 | 0x0200 | 0x0008, 0666);
    if (g_log >= 0) sceKernelWrite(g_log, b, n);
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;

    sceKernelMkdir("/data/jk", 0777);
    sceKernelMkdir(PASTA, 0777);

    logf_("DIAG: plugin_load comecou");

    struct proc_info pi; memset(&pi, 0, sizeof pi);
    int rc = sys_sdk_proc_info(&pi);
    logf_("DIAG: sys_sdk_proc_info rc=%d titulo=%s base=0x%llx",
          rc, pi.titleid, (unsigned long long)pi.base_address);

    logf_("DIAG: chegou ate o fim do plugin_load sem travar. hook_em seria 0x%llx",
          (unsigned long long)(pi.base_address + 0x311c90ULL));

    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    logf_("DIAG: plugin_unload chamado");
    return 0;
}

int32_t attr_module_hidden module_start(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
int32_t attr_module_hidden module_stop(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
