/* Modulo (.prx) de TESTE: so serve para descobrir se o PS4 deixa o app carregar um modulo de fora do codigo principal.
 * Nao faz parte do app de verdade. Gerado por: make build/jkmod_test.prx
 * (module_start/module_stop ja vem do crtlib.o do toolchain.) */

int __attribute__((visibility("default"))) jkmod_entry(int x) { return x + 1; }
