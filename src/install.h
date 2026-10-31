/* Instalacao via payload do DirectPackageInstaller (mesmo protocolo que ja funciona no PS4):
 *  1) envia o payload ao BinLoader do GoldHEN (porta 9090) com IP/porta de retorno no marcador B4;
 *  2) o payload conecta em 127.0.0.1:9091 e espera;
 *  3) entregamos o pacote no formato binario lido por get_pkg_info(). */
#ifndef INSTALL_H
#define INSTALL_H

void install_log(const char *msg);                       /* grava em /data/ps4store.log */
void install_start_server(void);                          /* thread que recebe a conexao do payload */
/* Normaliza o tipo (PS4GD, PS4GP, PS4AC...). Invalido ou vazio vira PS4GD. out >= 8 bytes. */
void install_sanitize_type(const char *in, char *out);
/* Retorna 0 em sucesso; <0 em erro, com a mensagem em err. Bloqueia ate ~15 s. */
int  install_pkg(const char *url, const char *name, const char *id, const char *type,
                 unsigned long long size, char *err, int errsz);

#endif
