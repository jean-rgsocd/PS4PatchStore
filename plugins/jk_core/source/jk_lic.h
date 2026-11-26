/* jk_lic.h - verificacao OFFLINE da autorizacao assinada JK PATCHS (Ed25519, TweetNaCl).
 *
 * Arquivo /data/jk/lic/<modulo>.lic (gravado pelo app a partir do /lic/sync):
 *   JKL1|<modulo>|<cid>|<uh>|<versao minima ou ->|<emitida>|<vence>|<assinatura hex 128>
 *   cid = 24 primeiros hex de SHA-512("jkcid:" + id do console)
 *
 * Sem dependencia do PS4: quem chama le os arquivos e passa o horario atual. */
#ifndef JK_LIC_H
#define JK_LIC_H
#include <stddef.h>
#include <stdint.h>

enum {
  JK_LIC_OK = 0,
  JK_LIC_FORMATO = 1,      /* arquivo ausente, truncado ou com campos errados */
  JK_LIC_ASSINATURA = 2,   /* assinatura nao confere com a chave publica */
  JK_LIC_MODULO = 3,       /* autorizacao de outro modulo */
  JK_LIC_CONSOLE = 4,      /* autorizacao de outro console */
  JK_LIC_VENCIDA = 5,      /* passou da validade (precisa abrir o app com internet) */
  JK_LIC_RELOGIO = 6,      /* relogio do console antes do que ja foi visto (data voltada) */
  JK_LIC_VERSAO = 7        /* versao da PRX abaixo da minima exigida */
};

/* conteudo/n: texto do .lic.  dev: id do console (texto hex, como o app envia).
 * agora: segundos (epoch).  visto: maior horario ja visto por esta PRX (0 se nao sabe) -
 * protege contra voltar a data do PS4 para "esticar" uma autorizacao vencida.
 * vence_out (opcional): recebe a validade.  Devolve JK_LIC_*. */
int jk_lic_checar(const char *conteudo, size_t n, const unsigned char pub[32], const char *modulo,
                  const char *versao, const char *dev, uint64_t agora, uint64_t visto, uint64_t *vence_out);

const char *jk_lic_motivo(int codigo);   /* texto curto, sem nada sensivel (pode ir para o log do cliente) */

/* cid do console: 24 hex + '\0' em out[25] */
void jk_lic_cid(const char *dev, char out[25]);
#endif
