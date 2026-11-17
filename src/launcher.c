/* JK Patchs - LAUNCHER. Quase nunca muda. Carrega o modulo da interface (jkui.prx) e chama jkui_main().
 * Ordem: /data/jk/jkui.prx (atualizavel pelo app/painel) e, se falhar, /app0/jkui.prx (copia que veio no PKG).
 * Protecao contra modulo ruim: antes de entrar no modulo grava /data/jk/boot.pending; o modulo apaga esse
 * arquivo quando a UI sobe. Se na proxima abertura o arquivo ainda existe, o modulo anterior travou:
 * volta para o .bak (ou para o /app0). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <orbis/libkernel.h>

uint64_t sceLibcHeapSize = 256ull * 1024 * 1024;

extern int jk_load_start_module(const char *name, size_t argc, const void *argv, unsigned flags, void *opt, int *res) __asm__("sceKernelLoadStartModule");
extern int jk_dlsym(int handle, const char *sym, void **addr) __asm__("sceKernelDlsym");

#ifndef JK_DIR
#define JK_DIR "/data/jk"      /* o app de teste do rollback usa outra pasta (/data/jkt) para nao mexer no app real */
#endif
#define DIR_JK  JK_DIR
#define MOD     DIR_JK "/jkui.prx"
#define MOD_NEW DIR_JK "/jkui.prx.new"
#define MOD_BAK DIR_JK "/jkui.prx.bak"
#define MOD_BAD DIR_JK "/jkui.prx.bad"
#define PEND    DIR_JK "/boot.pending"
#define MOD_APP "/app0/jkui.prx"
#define LAUNCHER_VERSION "2.1"

static void lg(const char *m) {
  FILE *f = fopen("/data/ps4store.log", "a");
  if (!f) return;
  fprintf(f, "%s\n", m);
  fclose(f);
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static int try_run(const char *path, int guard) {
  char b[320];
  if (!exists(path)) return -1;
  chmod(path, 0777);                         /* arquivo criado pelo app nasce sem permissao de execucao */
  if (guard) { FILE *f = fopen(PEND, "w"); if (f) { fputs("1", f); fclose(f); } }
  int res = 0;
  int h = jk_load_start_module(path, 0, NULL, 0, NULL, &res);
  if (h < 0) {
    struct stat st; unsigned md = stat(path, &st) == 0 ? (unsigned)st.st_mode : 0;
    snprintf(b, sizeof(b), "LAUNCHER: falha ao carregar %s (0x%08x) modo=%o tam=%lld", path, (unsigned)h, md, (long long)st.st_size); lg(b);
    if (guard) unlink(PEND);
    return -1;
  }
  void *fn = NULL;
  if (jk_dlsym(h, "jkui_main", &fn) != 0 || !fn) {
    snprintf(b, sizeof(b), "LAUNCHER: %s sem jkui_main", path); lg(b);
    if (guard) unlink(PEND);
    return -1;
  }
  void *vf = NULL;
  if (jk_dlsym(h, "jkui_version", &vf) == 0 && vf) {
    snprintf(b, sizeof(b), "LAUNCHER: rodando %s (modulo v%s)", path, ((const char *(*)(void))vf)()); lg(b);
  }
  int rc = ((int (*)(void))fn)();
  if (guard) unlink(PEND);
  return rc;
}

int main(void) {
  lg("=== JK PATCHS LAUNCHER v" LAUNCHER_VERSION " ===");
  mkdir(DIR_JK, 0777);

  if (exists(PEND)) {                       /* a abertura anterior travou dentro do modulo */
    lg("LAUNCHER: modulo anterior nao subiu; revertendo.");
    unlink(PEND);
    unlink(MOD_BAD);
    if (exists(MOD)) rename(MOD, MOD_BAD);
    if (exists(MOD_BAK)) rename(MOD_BAK, MOD);
    unlink(MOD_NEW);
  } else if (exists(MOD_NEW)) {             /* modulo novo baixado: troca, guardando o atual como backup */
    lg("LAUNCHER: instalando modulo novo.");
    unlink(MOD_BAK);
    if (exists(MOD)) rename(MOD, MOD_BAK);
    rename(MOD_NEW, MOD);
  }

  int rc = try_run(MOD, 1);
  if (rc == -1) rc = try_run(MOD_APP, 0);
  if (rc == -1) lg("LAUNCHER: nenhum modulo pode ser carregado.");
  return rc == -1 ? 1 : rc;
}
