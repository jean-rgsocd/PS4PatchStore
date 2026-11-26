#include "qrcode.h"
#include <string.h>

/* ---- tabelas (ISO 18004): codewords de correcao por bloco e numero de blocos, indice = versao */
static const int ECC_PER_BLOCK[2][QR_MAX_VERSION + 1] = {
  {0, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22},   /* L */
  {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24},  /* M */
};
static const int NUM_BLOCKS[2][QR_MAX_VERSION + 1] = {
  {0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6},                 /* L */
  {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10},                /* M */
};

static int raw_data_modules(int ver) {
  int result = (16 * ver + 128) * ver + 64;
  if (ver >= 2) {
    int na = ver / 7 + 2;
    result -= (25 * na - 10) * na - 55;
    if (ver >= 7) result -= 36;
  }
  return result;
}

static int data_codewords(int ver, int ecc) {
  return raw_data_modules(ver) / 8 - ECC_PER_BLOCK[ecc][ver] * NUM_BLOCKS[ecc][ver];
}

/* ---- Reed-Solomon sobre GF(256) (polinomio 0x11D) */
static uint8_t gf_mul(uint8_t x, uint8_t y) {
  int z = 0;
  for (int i = 7; i >= 0; i--) {
    z = (z << 1) ^ ((z >> 7) * 0x11D);
    z ^= ((y >> i) & 1) * x;
  }
  return (uint8_t)z;
}

static void rs_divisor(int degree, uint8_t *result) {
  memset(result, 0, (size_t)degree);
  result[degree - 1] = 1;
  uint8_t root = 1;
  for (int i = 0; i < degree; i++) {
    for (int j = 0; j < degree; j++) {
      result[j] = gf_mul(result[j], root);
      if (j + 1 < degree) result[j] ^= result[j + 1];
    }
    root = gf_mul(root, 0x02);
  }
}

static void rs_remainder(const uint8_t *data, int len, const uint8_t *div, int degree, uint8_t *result) {
  memset(result, 0, (size_t)degree);
  for (int i = 0; i < len; i++) {
    uint8_t factor = data[i] ^ result[0];
    memmove(result, result + 1, (size_t)degree - 1);
    result[degree - 1] = 0;
    for (int j = 0; j < degree; j++) result[j] ^= gf_mul(div[j], factor);
  }
}

/* ---- matriz */
typedef struct {
  int size;
  uint8_t *m;          /* 1 = escuro */
  uint8_t *fn;         /* 1 = modulo de funcao (nao recebe dados nem mascara) */
} Qr;

static void setfn(Qr *q, int x, int y, int dark) {
  if (x < 0 || y < 0 || x >= q->size || y >= q->size) return;
  q->m[y * q->size + x] = (uint8_t)dark;
  q->fn[y * q->size + x] = 1;
}

static void draw_finder(Qr *q, int cx, int cy) {
  for (int dy = -4; dy <= 4; dy++)
    for (int dx = -4; dx <= 4; dx++) {
      int d = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
      setfn(q, cx + dx, cy + dy, d != 2 && d != 4);
    }
}

static void draw_align(Qr *q, int cx, int cy) {
  for (int dy = -2; dy <= 2; dy++)
    for (int dx = -2; dx <= 2; dx++) {
      int d = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
      setfn(q, cx + dx, cy + dy, d != 1);
    }
}

static int align_positions(int ver, int *pos) {
  if (ver == 1) return 0;
  int n = ver / 7 + 2;
  int step = (ver == 32) ? 26 : (ver * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
  pos[0] = 6;
  for (int i = n - 1, p = ver * 4 + 10; i >= 1; i--, p -= step) pos[i] = p;
  return n;
}

static void draw_format(Qr *q, int ecc, int mask) {
  int fmt_ecc = ecc == 0 ? 1 : 0;                       /* L = 01, M = 00 */
  int data = fmt_ecc << 3 | mask;
  int rem = data;
  for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
  int bits = (data << 10 | rem) ^ 0x5412;
  for (int i = 0; i <= 5; i++) setfn(q, 8, i, (bits >> i) & 1);
  setfn(q, 8, 7, (bits >> 6) & 1);
  setfn(q, 8, 8, (bits >> 7) & 1);
  setfn(q, 7, 8, (bits >> 8) & 1);
  for (int i = 9; i < 15; i++) setfn(q, 14 - i, 8, (bits >> i) & 1);
  for (int i = 0; i < 8; i++) setfn(q, q->size - 1 - i, 8, (bits >> i) & 1);
  for (int i = 8; i < 15; i++) setfn(q, 8, q->size - 15 + i, (bits >> i) & 1);
  setfn(q, 8, q->size - 8, 1);
}

static void draw_function(Qr *q, int ver, int ecc) {
  for (int i = 0; i < q->size; i++) { setfn(q, 6, i, i % 2 == 0); setfn(q, i, 6, i % 2 == 0); }
  draw_finder(q, 3, 3); draw_finder(q, q->size - 4, 3); draw_finder(q, 3, q->size - 4);
  int pos[10];
  int n = align_positions(ver, pos);
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++)
      if (!((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0))) draw_align(q, pos[i], pos[j]);
  draw_format(q, ecc, 0);
  if (ver >= 7) {
    int rem = ver;
    for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    long bits = (long)ver << 12 | rem;
    for (int i = 0; i < 18; i++) {
      int bit = (int)((bits >> i) & 1);
      int a = q->size - 11 + i % 3, b = i / 3;
      setfn(q, a, b, bit); setfn(q, b, a, bit);
    }
  }
}

static void draw_codewords(Qr *q, const uint8_t *data, int ndata) {
  int i = 0;
  for (int right = q->size - 1; right >= 1; right -= 2) {
    if (right == 6) right = 5;
    for (int vert = 0; vert < q->size; vert++)
      for (int j = 0; j < 2; j++) {
        int x = right - j;
        int upward = ((right + 1) & 2) == 0;
        int y = upward ? q->size - 1 - vert : vert;
        if (!q->fn[y * q->size + x] && i < ndata * 8) {
          q->m[y * q->size + x] = (data[i >> 3] >> (7 - (i & 7))) & 1;
          i++;
        }
      }
  }
}

static void apply_mask(Qr *q, int mask) {
  for (int y = 0; y < q->size; y++)
    for (int x = 0; x < q->size; x++) {
      int inv;
      switch (mask) {
        case 0: inv = (x + y) % 2 == 0; break;
        case 1: inv = y % 2 == 0; break;
        case 2: inv = x % 3 == 0; break;
        case 3: inv = (x + y) % 3 == 0; break;
        case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
        case 5: inv = x * y % 2 + x * y % 3 == 0; break;
        case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
        default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
      }
      if (inv && !q->fn[y * q->size + x]) q->m[y * q->size + x] ^= 1;
    }
}

static int penalty(const Qr *q) {
  int n = q->size, total = 0;
  for (int pass = 0; pass < 2; pass++) {                 /* linhas e colunas */
    for (int a = 0; a < n; a++) {
      int run = 1;
      for (int b = 1; b < n; b++) {
        int cur = pass ? q->m[b * n + a] : q->m[a * n + b];
        int prev = pass ? q->m[(b - 1) * n + a] : q->m[a * n + b - 1];
        if (cur == prev) { run++; if (run == 5) total += 3; else if (run > 5) total++; }
        else run = 1;
      }
      for (int b = 0; b + 10 < n; b++) {                  /* padrao parecido com o localizador */
        int c1 = 1, c2 = 1;
        static const int P1[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
        static const int P2[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
        for (int k = 0; k < 11; k++) {
          int v = pass ? q->m[(b + k) * n + a] : q->m[a * n + b + k];
          if (v != P1[k]) c1 = 0;
          if (v != P2[k]) c2 = 0;
        }
        total += (c1 + c2) * 40;
      }
    }
  }
  for (int y = 0; y + 1 < n; y++)
    for (int x = 0; x + 1 < n; x++) {
      int c = q->m[y * n + x];
      if (c == q->m[y * n + x + 1] && c == q->m[(y + 1) * n + x] && c == q->m[(y + 1) * n + x + 1]) total += 3;
    }
  int dark = 0;
  for (int i = 0; i < n * n; i++) dark += q->m[i];
  int k = 0, t = n * n;
  int diff = dark * 20 - t * 10;
  if (diff < 0) diff = -diff;
  while (diff > t) { diff -= t; k++; }                    /* 5% de desvio = 10 pontos */
  total += k * 10;
  return total;
}

int qr_encode(const char *text, int ecc, uint8_t modules[QR_MAX_SIZE * QR_MAX_SIZE]) {
  if (ecc < 0 || ecc > 1) ecc = 1;
  int len = (int)strlen(text);
  int ver = 0, cap_bits = 0;
  for (int v = 1; v <= QR_MAX_VERSION; v++) {
    cap_bits = data_codewords(v, ecc) * 8;
    int cc = v < 10 ? 8 : 16;
    if (4 + cc + len * 8 <= cap_bits) { ver = v; break; }
  }
  if (!ver) return 0;
  int ndata = data_codewords(ver, ecc);
  int cc = ver < 10 ? 8 : 16;

  uint8_t buf[512];
  memset(buf, 0, sizeof(buf));
  int bl = 0;
#define PUT(val, cnt) do { for (int _i = (cnt) - 1; _i >= 0; _i--) { if ((((val) >> _i) & 1)) buf[bl >> 3] |= (uint8_t)(0x80 >> (bl & 7)); bl++; } } while (0)
  PUT(4, 4);
  PUT(len, cc);
  for (int i = 0; i < len; i++) PUT((unsigned char)text[i], 8);
  int term = cap_bits - bl; if (term > 4) term = 4;
  bl += term;
  bl = (bl + 7) & ~7;
  for (uint8_t pad = 0xEC; bl < cap_bits; pad ^= 0xEC ^ 0x11) { buf[bl >> 3] = pad; bl += 8; }
#undef PUT

  /* blocos + correcao de erro, intercalados */
  int nb = NUM_BLOCKS[ecc][ver], eb = ECC_PER_BLOCK[ecc][ver];
  int raw = raw_data_modules(ver) / 8;
  int nshort = nb - raw % nb, sblen = raw / nb;
  uint8_t div[32], rem[32];
  rs_divisor(eb, div);
  static uint8_t all[1024];
  const uint8_t *blk[16];
  int blklen[16];
  uint8_t eccs[16][32];
  int k = 0;
  for (int i = 0; i < nb; i++) {
    int dl = sblen - eb + (i < nshort ? 0 : 1);
    blk[i] = buf + k; blklen[i] = dl;
    rs_remainder(buf + k, dl, div, eb, rem);
    memcpy(eccs[i], rem, (size_t)eb);
    k += dl;
  }
  int o = 0;
  for (int i = 0; i < sblen + 1; i++)
    for (int j = 0; j < nb; j++)
      if (i != sblen - eb || j >= nshort) { if (i < blklen[j]) all[o++] = blk[j][i]; }
  for (int i = 0; i < eb; i++)
    for (int j = 0; j < nb; j++) all[o++] = eccs[j][i];
  (void)ndata;

  static uint8_t fn[QR_MAX_SIZE * QR_MAX_SIZE];
  Qr q;
  q.size = 17 + 4 * ver;
  q.m = modules; q.fn = fn;
  memset(modules, 0, (size_t)q.size * q.size);
  memset(fn, 0, (size_t)q.size * q.size);
  draw_function(&q, ver, ecc);
  draw_codewords(&q, all, o);

  int best = 0, bestp = 1 << 30;
  for (int mk = 0; mk < 8; mk++) {
    apply_mask(&q, mk);
    draw_format(&q, ecc, mk);
    int p = penalty(&q);
    if (p < bestp) { bestp = p; best = mk; }
    apply_mask(&q, mk);                                   /* desfaz */
  }
  apply_mask(&q, best);
  draw_format(&q, ecc, best);
  return q.size;
}
