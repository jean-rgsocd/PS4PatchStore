/*
 * JK EXTRATOR v1.01 - PES 2021 (CUSA18676)
 * Le o indice (TOC) de todos os .cpk do jogo (/app0) e do AFR (/data/GoldHEN/AFR/CUSA18676)
 * e procura arquivos pelo nome. Nao mexe em nada do jogo.
 *
 * Procura: cada linha de /data/jk/super/procurar.txt (pedaco do caminho, ex.: bill/).
 *          Sem o arquivo, procura "bill".
 * Copia:   cada linha de /data/jk/super/copiar_arquivos.txt (pedaco do caminho).
 *          Sem o arquivo, copia o que tiver "bill/config" ou "demoarea_bill".
 * Saida:   /data/jk/super/extrator.log (lista: cpk | caminho | tamanho | tamanho_real)
 *          /data/jk/super/extrair/<cpk>__<caminho_com__>  (bytes como estao no cpk)
 * Roda 1x. Para rodar de novo, apague /data/jk/super/extrator_feito.txt
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>
#include "GoldHEN.h"

#define attr_public __attribute__((visibility("default")))
#define attr_module_hidden __attribute__((weak)) __attribute__((visibility("hidden")))

attr_public const char *g_pluginName = "jk_extrator";
attr_public const char *g_pluginDesc = "JK PATCHS - extrator de arquivos dos CPKs (so leitura)";
attr_public const char *g_pluginAuth = "JK PATCHS";
attr_public uint32_t g_pluginVersion = 0x00000101;

int64_t jk_pread(int fd, void *buf, uint64_t n, int64_t off) __asm__("sceKernelPread");
int jk_getdents(int fd, char *buf, int n) __asm__("sceKernelGetdents");

#define PASTA "/data/jk/super"
#define SAIDA PASTA "/extrair"
#define MAX_PAD 32
#define LIMITE_COPIA (600ULL * 1024 * 1024)

static OrbisPthread g_thr;
static void *trabalho(void *arg);

/* plugin_load tem de ser a PRIMEIRA funcao do arquivo: o console comeca a executar pelo inicio do .text */
int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    scePthreadCreate(&g_thr, 0, trabalho, 0, "jk_extrator");
    return 0;
}
static int g_log = -1;
static char g_busca[MAX_PAD][96]; static int g_nbusca;
static char g_copia[MAX_PAD][96]; static int g_ncopia;
static uint64_t g_copiado;
static int g_achados, g_copias;

static void logf_(const char *fmt, ...)
{
    char b[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    if (g_log < 0) g_log = sceKernelOpen(PASTA "/extrator.log", 0x0001 | 0x0200 | 0x0400, 0666);
    if (g_log >= 0) sceKernelWrite(g_log, b, n);
}

static int existe(const char *p) { OrbisKernelStat st; return sceKernelStat(p, &st) == 0; }

static int ler_lista(const char *arq, char out[][96])
{
    int fd = sceKernelOpen(arq, 0, 0);
    if (fd < 0) return 0;
    char t[4096]; int n = (int)sceKernelRead(fd, t, sizeof t - 1); sceKernelClose(fd);
    if (n <= 0) return 0;
    t[n] = 0;
    int k = 0; char *p = t;
    while (*p && k < MAX_PAD) {
        char *e = p; while (*e && *e != '\n' && *e != '\r') e++;
        int l = (int)(e - p);
        if (l > 0 && l < 95 && p[0] != '#') { memcpy(out[k], p, l); out[k][l] = 0; k++; }
        p = *e ? e + 1 : e;
    }
    return k;
}

static int contem(const char *s, const char *pad)
{
    size_t lp = strlen(pad);
    for (; *s; s++) {
        size_t i = 0;
        while (i < lp && s[i]) {
            char a = s[i], b = pad[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            i++;
        }
        if (i == lp) return 1;
    }
    return 0;
}

/* ---------- @UTF da CRI ---------- */
static void cri_dec(uint8_t *b, size_t n)
{
    uint32_t m = 0x655f;
    for (size_t i = 0; i < n; i++) { b[i] ^= (uint8_t)m; m = m * 0x4115; }
}
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

typedef struct { const char *nome; uint8_t st, ty; const uint8_t *cst; } col_t;
typedef struct {
    uint8_t *mem; const uint8_t *t; uint32_t tam;
    uint32_t rows_off, str_off, data_off; uint16_t ncol, rowlen; uint32_t nrows;
    col_t col[64];
} utf_t;

static int tam_tipo(uint8_t ty) { switch (ty) { case 0: case 1: return 1; case 2: case 3: return 2; case 4: case 5: case 8: case 0xa: return 4; case 6: case 7: case 0xb: return 8; } return -1; }

/* le um @UTF que comeca em 'off' do arquivo (criptografado ou nao) */
static int utf_abrir(int fd, int64_t off, utf_t *u)
{
    memset(u, 0, sizeof *u);
    uint8_t h[8];
    if (jk_pread(fd, h, 8, off) != 8) return -1;
    int cripto = memcmp(h, "@UTF", 4) != 0;
    if (cripto) cri_dec(h, 8);
    if (memcmp(h, "@UTF", 4) != 0) return -2;
    uint32_t tam = be32(h + 4);
    if (tam < 24 || tam > 256u * 1024 * 1024) return -3;
    uint8_t *m = malloc(tam + 8);
    if (!m) return -4;
    if (jk_pread(fd, m, tam + 8, off) != (int64_t)(tam + 8)) { free(m); return -5; }
    if (cripto) cri_dec(m, tam + 8);
    u->mem = m; u->t = m + 8; u->tam = tam;
    const uint8_t *t = u->t;
    u->rows_off = be16(t + 2); u->str_off = be32(t + 4); u->data_off = be32(t + 8);
    u->ncol = be16(t + 16); u->rowlen = be16(t + 18); u->nrows = be32(t + 20);
    if (u->ncol > 64) { free(m); return -6; }
    const uint8_t *p = t + 24;
    for (int c = 0; c < u->ncol; c++) {
        uint8_t fl = p[0]; uint32_t no = be32(p + 1); p += 5;
        u->col[c].nome = (const char *)(t + u->str_off + no);
        u->col[c].st = fl & 0xf0; u->col[c].ty = fl & 0x0f;
        if (u->col[c].st == 0x30) { u->col[c].cst = p; int s = tam_tipo(u->col[c].ty); if (s < 0) { free(m); return -7; } p += s; }
    }
    return 0;
}
static void utf_fechar(utf_t *u) { if (u->mem) free(u->mem); u->mem = 0; }

/* valor da coluna 'nome' na linha r: numero em *num, texto em *txt */
static int utf_val(utf_t *u, uint32_t r, const char *nome, uint64_t *num, const char **txt)
{
    const uint8_t *q = u->t + u->rows_off + (uint64_t)r * u->rowlen;
    for (int c = 0; c < u->ncol; c++) {
        col_t *cl = &u->col[c];
        const uint8_t *v = 0;
        if (cl->st == 0x50) { v = q; q += tam_tipo(cl->ty); }
        else if (cl->st == 0x30) v = cl->cst;
        if (strcmp(cl->nome, nome) != 0) continue;
        if (!v) return 0;
        switch (cl->ty) {
            case 0: case 1: *num = v[0]; break;
            case 2: case 3: *num = be16(v); break;
            case 4: case 5: *num = be32(v); break;
            case 6: case 7: *num = be64(v); break;
            case 0xa: if (txt) *txt = (const char *)(u->t + u->str_off + be32(v)); break;
            default: return 0;
        }
        return 1;
    }
    return 0;
}

/* ---------- copia ---------- */
static void copiar(int fd, const char *cpk, const char *caminho, int64_t off, uint64_t tam)
{
    if (g_copiado + tam > LIMITE_COPIA) { logf_("  (limite de copia atingido: %s)", caminho); return; }
    char nome[400]; int k = snprintf(nome, sizeof nome, SAIDA "/%s__", cpk);
    for (const char *s = caminho; *s && k < (int)sizeof nome - 1; s++) nome[k++] = (*s == '/') ? '_' : *s;
    nome[k] = 0;
    int o = sceKernelOpen(nome, 0x0001 | 0x0200 | 0x0400, 0666);
    if (o < 0) { logf_("  ERRO criar %s 0x%x", nome, o); return; }
    uint8_t *b = malloc(1 << 20);
    uint64_t feito = 0;
    while (b && feito < tam) {
        uint64_t n = tam - feito; if (n > (1 << 20)) n = 1 << 20;
        int64_t r = jk_pread(fd, b, n, off + (int64_t)feito);
        if (r <= 0) break;
        sceKernelWrite(o, b, (size_t)r); feito += (uint64_t)r;
    }
    if (b) free(b);
    sceKernelClose(o);
    g_copiado += feito; g_copias++;
}

/* ---------- um cpk ---------- */
static void varrer_cpk(const char *pasta, const char *arq)
{
    char cam[400]; snprintf(cam, sizeof cam, "%s/%s", pasta, arq);
    int fd = sceKernelOpen(cam, 0, 0);
    if (fd < 0) { logf_("# %s: nao abriu 0x%x", cam, fd); return; }
    uint8_t mg[4];
    if (jk_pread(fd, mg, 4, 0) != 4 || memcmp(mg, "CPK ", 4) != 0) { logf_("# %s: nao e CPK", cam); sceKernelClose(fd); return; }
    utf_t h;
    int r = utf_abrir(fd, 0x10, &h);
    if (r) { logf_("# %s: cabecalho ilegivel %d", cam, r); sceKernelClose(fd); return; }
    uint64_t toc = 0, cont = 0, nfiles = 0;
    utf_val(&h, 0, "TocOffset", &toc, 0); utf_val(&h, 0, "ContentOffset", &cont, 0); utf_val(&h, 0, "Files", &nfiles, 0);
    utf_fechar(&h);
    if (!toc) { logf_("# %s: sem TOC (so ITOC) arquivos=%llu", cam, (unsigned long long)nfiles); sceKernelClose(fd); return; }
    uint8_t tg[4];
    if (jk_pread(fd, tg, 4, (int64_t)toc) != 4 || memcmp(tg, "TOC ", 4) != 0) { logf_("# %s: TOC invalido", cam); sceKernelClose(fd); return; }
    utf_t t;
    r = utf_abrir(fd, (int64_t)toc + 0x10, &t);
    if (r) { logf_("# %s: TOC ilegivel %d", cam, r); sceKernelClose(fd); return; }
    uint64_t add = toc; if (cont && cont < toc) add = cont;
    int achou = 0;
    for (uint32_t i = 0; i < t.nrows; i++) {
        const char *dir = "", *fn = "";
        uint64_t fo = 0, fs = 0, es = 0, dummy;
        utf_val(&t, i, "DirName", &dummy, &dir); utf_val(&t, i, "FileName", &dummy, &fn);
        utf_val(&t, i, "FileOffset", &fo, 0); utf_val(&t, i, "FileSize", &fs, 0); utf_val(&t, i, "ExtractSize", &es, 0);
        char c2[400]; snprintf(c2, sizeof c2, "%s/%s", dir ? dir : "", fn ? fn : "");
        int bate = 0;
        for (int k = 0; k < g_nbusca; k++) if (contem(c2, g_busca[k])) { bate = 1; break; }
        if (!bate) continue;
        achou++; g_achados++;
        int cp = 0;
        for (int k = 0; k < g_ncopia; k++) if (contem(c2, g_copia[k])) { cp = 1; break; }
        logf_("%s | %s | %llu | %llu%s", arq, c2, (unsigned long long)fs, (unsigned long long)es, cp ? " | COPIADO" : "");
        if (cp) copiar(fd, arq, c2, (int64_t)(fo + add), fs);
    }
    logf_("# %s: %u arquivos no indice, %d achados", arq, t.nrows, achou);
    utf_fechar(&t);
    sceKernelClose(fd);
}

static void varrer_pasta(const char *pasta)
{
    int fd = sceKernelOpen(pasta, 0x00020000 /* O_DIRECTORY */, 0);
    if (fd < 0) { logf_("## pasta %s nao abriu 0x%x", pasta, fd); return; }
    logf_("## pasta %s", pasta);
    static char nomes[256][128]; int nn = 0;
    char buf[4096];
    for (;;) {
        int n = jk_getdents(fd, buf, sizeof buf);
        if (n <= 0) break;
        for (int p = 0; p < n;) {
            uint16_t reclen = *(uint16_t *)(buf + p + 4);
            uint8_t namlen = *(uint8_t *)(buf + p + 7);
            const char *nm = buf + p + 8;
            if (reclen == 0) break;
            if (namlen > 4 && namlen < 127 && !memcmp(nm + namlen - 4, ".cpk", 4) && nn < 256) { memcpy(nomes[nn], nm, namlen); nomes[nn][namlen] = 0; nn++; }
            p += reclen;
        }
    }
    sceKernelClose(fd);
    for (int i = 0; i < nn; i++) varrer_cpk(pasta, nomes[i]);
}

static void *trabalho(void *arg)
{
    (void)arg;
    sceKernelMkdir("/data/jk", 0777); sceKernelMkdir(PASTA, 0777); sceKernelMkdir(SAIDA, 0777);
    if (existe(PASTA "/extrator_feito.txt")) return 0;
    struct proc_info pi; memset(&pi, 0, sizeof pi);
    sys_sdk_proc_info(&pi);
    if (strncmp(pi.titleid, "CUSA18676", 9) != 0) return 0;
    sceKernelUsleep(15 * 1000000);                      /* deixa o jogo abrir primeiro */
    g_nbusca = ler_lista(PASTA "/procurar.txt", g_busca);
    if (!g_nbusca) {                                    /* placas de publicidade: varios nomes possiveis */
        static const char *pad[] = { "bill", "adboard", "ad_board", "/ad/", "banner", "advert", "comp_ad", "signboard" };
        for (int i = 0; i < (int)(sizeof pad / sizeof pad[0]); i++) strcpy(g_busca[i], pad[i]);
        g_nbusca = (int)(sizeof pad / sizeof pad[0]);
    }
    g_ncopia = ler_lista(PASTA "/copiar_arquivos.txt", g_copia);
    if (!g_ncopia) {                                    /* so os arquivos pequenos de configuracao */
        static const char *pad[] = { "bill/config", "demoarea_bill", "adboard/config", "ad/config", ".xml", ".json", ".bin" };
        for (int i = 0; i < (int)(sizeof pad / sizeof pad[0]); i++) strcpy(g_copia[i], pad[i]);
        g_ncopia = (int)(sizeof pad / sizeof pad[0]);
    }
    logf_("JK_EXTRATOR v1.01 busca=%d copia=%d (formato: cpk | caminho | tamanho_no_cpk | tamanho_real)", g_nbusca, g_ncopia);
    varrer_pasta("/app0");
    varrer_pasta("/data/GoldHEN/AFR/CUSA18676");
    logf_("FIM achados=%d copiados=%d bytes=%llu", g_achados, g_copias, (unsigned long long)g_copiado);
    int f = sceKernelOpen(PASTA "/extrator_feito.txt", 0x0001 | 0x0200 | 0x0400, 0666);
    if (f >= 0) { sceKernelWrite(f, "ok\n", 3); sceKernelClose(f); }
    if (g_log >= 0) { sceKernelClose(g_log); g_log = -1; }
    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[]) { (void)argc; (void)argv; return 0; }
int32_t attr_module_hidden module_start(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
int32_t attr_module_hidden module_stop(size_t argc, const void *args) { (void)argc; (void)args; return 0; }
