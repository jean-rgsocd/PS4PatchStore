/* chave PUBLICA das licencas JK PATCHS (Ed25519).
 *
 * ESTE E O ARQUIVO PROVISORIO: troque pelo lic_pub.h gerado por
 *   node plataforma/central/gerar_chaves.mjs <pasta>
 * (a chave PRIVADA do mesmo par vai no secret LIC_PRIV do Cloudflare).
 *
 * Enquanto JK_LIC_PUB_PENDENTE existir, a PRX NAO ativa o VAR (falha fechada) e grava no log:
 *   "VAR desligado: build sem chave publica". */
#ifndef JK_LIC_PUB_H
#define JK_LIC_PUB_H
#define JK_LIC_PUB_PENDENTE 1
static const unsigned char JK_LIC_PUB[32] = { 0 };
#endif
