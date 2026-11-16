/* Modulo (.prx) de TESTE: so serve para descobrir se o PS4 deixa o app carregar um modulo de fora do codigo principal.
 * Nao faz parte do app de verdade. Gerado por: make build/jkmod_test.prx */
#include <stddef.h>

int __attribute__((visibility("default"))) module_start(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
int __attribute__((visibility("default"))) module_stop(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
int __attribute__((visibility("default"))) jkmod_entry(int x) { return x + 1; }
