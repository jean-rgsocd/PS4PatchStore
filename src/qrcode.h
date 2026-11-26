/* Gerador de QR Code (modo byte, versoes 1 a 15, correcao de erro L/M). Sem dependencias: serve para o Pix e o WhatsApp
 * serem desenhados direto na tela, com o valor do plano escolhido. */
#ifndef QRCODE_H
#define QRCODE_H

#include <stdint.h>

#define QR_MAX_VERSION 15
#define QR_MAX_SIZE    (17 + 4 * QR_MAX_VERSION)      /* 77 modulos de lado */

/* ecc: 0 = L (7%), 1 = M (15%). modules recebe size*size bytes (1 = escuro). Retorna o lado (size) ou 0 se nao coube. */
int qr_encode(const char *text, int ecc, uint8_t modules[QR_MAX_SIZE * QR_MAX_SIZE]);

#endif
