/* jk_modulo.h - modulo cifrado do JK Core (.jkm). XSalsa20 (TweetNaCl) + SHA-512 para conferir.
 *
 * Formato (.jkm):
 *   "JKM1" | u8 n | modulo[n] | u8 t | titulos[t] ("CUSA18676,CUSA22062") | nonce[24] |
 *   u32 tamanho (little endian) | sha512(prx)[16 primeiros] | prx cifrado[tamanho]
 * A chave (32 bytes) NAO vai no arquivo: chega junto com a licenca (/data/jk/lic/<modulo>.key). */
#ifndef JK_MODULO_H
#define JK_MODULO_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char modulo[41];
  char titulos[161];
  const unsigned char *nonce;      /* 24 bytes, aponta para dentro do buffer */
  uint32_t tamanho;
  const unsigned char *hash16;
  const unsigned char *cifrado;    /* aponta para dentro do buffer */
} jkm_info;

enum { JKM_OK = 0, JKM_FORMATO = 1, JKM_TITULO = 2, JKM_CHAVE = 3, JKM_INTEGRIDADE = 4 };

int jkm_ler(const unsigned char *buf, size_t n, jkm_info *out);           /* le o cabecalho */
int jkm_titulo_ok(const jkm_info *i, const char *titulo);                 /* 1 se o jogo esta na lista */
int jkm_decifrar(const jkm_info *i, const unsigned char chave[32], unsigned char *saida);   /* saida[tamanho] */
int jkm_chave_hex(const char *txt, size_t n, unsigned char chave[32]);    /* 64 hex -> 32 bytes; 0 = ok */
/* monta um .jkm (ferramenta do PC). saida precisa de jkm_tamanho_cifrado(...) bytes. Devolve o tamanho. */
size_t jkm_tamanho_cifrado(const char *modulo, const char *titulos, uint32_t tamanho);
size_t jkm_montar(const char *modulo, const char *titulos, const unsigned char nonce[24], const unsigned char chave[32],
                  const unsigned char *prx, uint32_t tamanho, unsigned char *saida);
#endif
