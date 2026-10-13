// tests/codec2_frame_codec.c
//
// Encode/decode a raw 8 kHz signed 16 bit mono file with ONE Codec2 back end,
// dumping the encoded bitstream and the decoded audio for later comparison.
// Compiled twice by "make bitexact":
//
//   codec2_frame_stock  links the stock libcodec2
//   codec2_frame_mod    links the vendored modified Codec2 (codec2mod/)
//
// They are deliberately separate binaries: libcodec2 exports internal symbols
// (kiss_fftr, analyse_one_frame, ...) with the same names as the modified
// codec, and an executable's definitions take precedence when resolving the
// shared library's calls, so linking both into one process makes the stock
// encoder call into the mod's internals and crash.
//
//   usage: codec2_frame_<x> input.raw out.c2 out.raw [max-frames]
//
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef CODEC2_MOD
  #define CODEC2_SHIM_INTERNAL 1
  #include "codec2_shim.h"
  static const char *codec_name = "mod (M17-Project/Codec2-mod)";
  static struct CODEC2 *mk(int mode)                  { return codec2m_create(mode); }
  static int     n_samples(struct CODEC2 *c)          { return codec2m_samples_per_frame(c); }
  static int     n_bytes(struct CODEC2 *c)            { return codec2m_bytes_per_frame(c); }
  static void    enc(struct CODEC2 *c, uint8_t *b, int16_t *s) { codec2m_encode(c, b, s); }
  static void    dec(struct CODEC2 *c, int16_t *s, const uint8_t *b) { codec2m_decode(c, s, b); }
#else
  #include "codec2.h"
  // Upstream codec2.h only declares "struct CODEC2" - it has no CODEC2 typedef,
  // which is why the m17web sources (C++) can write CODEC2 and C cannot.
  static const char *codec_name = "stock libcodec2";
  static struct CODEC2 *mk(int mode)                  { return codec2_create(mode); }
  static int     n_samples(struct CODEC2 *c)          { return codec2_samples_per_frame(c); }
  static int     n_bytes(struct CODEC2 *c)            { return codec2_bits_per_frame(c) / 8; }
  static void    enc(struct CODEC2 *c, uint8_t *b, int16_t *s) { codec2_encode(c, b, s); }
  static void    dec(struct CODEC2 *c, int16_t *s, const uint8_t *b) { codec2_decode(c, s, b); }
#endif

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 4) {
        fprintf(stderr, "usage: %s input.raw out.c2 out.raw [max-frames]\n", argv[0]);
        return 1;
    }
    const char *in_path = argv[1], *c2_path = argv[2], *raw_path = argv[3];
    int max_frames = (argc > 4) ? atoi(argv[4]) : 500;

    FILE *fi = fopen(in_path, "rb");
    if (!fi) { perror(in_path); return 1; }

    struct CODEC2 *c = mk(CODEC2_MODE_3200);
    if (!c) { fprintf(stderr, "codec2_create failed\n"); return 1; }
    const int NS = n_samples(c), NB = n_bytes(c);

    printf("codec   : %s\n", codec_name);
    printf("frame   : %d samples (%d ms) / %d bytes (%d bits)\n",
           NS, (NS * 1000) / 8000, NB, NB * 8);

    int16_t *speech = malloc(sizeof(int16_t) * (size_t)NS * max_frames);
    uint8_t *bits   = malloc((size_t)NB * max_frames);
    int16_t *decoded = malloc(sizeof(int16_t) * (size_t)NS * max_frames);
    if (!speech || !bits || !decoded) { fprintf(stderr, "out of memory\n"); return 1; }

    int n = 0;
    while (n < max_frames && fread(&speech[n * NS], sizeof(int16_t), NS, fi) == (size_t)NS) n++;
    fclose(fi);
    if (n == 0) { fprintf(stderr, "no audio read\n"); return 1; }

    double t0 = now_s();
    for (int i = 0; i < n; i++) enc(c, &bits[i * NB], &speech[i * NS]);
    double t_enc = now_s() - t0;

    t0 = now_s();
    for (int i = 0; i < n; i++) dec(c, &decoded[i * NS], &bits[i * NB]);
    double t_dec = now_s() - t0;

    FILE *fb = fopen(c2_path, "wb");
    FILE *fr = fopen(raw_path, "wb");
    if (!fb || !fr) { perror(c2_path); return 1; }
    fwrite(bits, 1, (size_t)NB * n, fb);
    fwrite(decoded, sizeof(int16_t), (size_t)NS * n, fr);
    fclose(fb); fclose(fr);

    printf("frames  : %d (%.2f s of audio)\n", n, n * 0.02);
    printf("timing  : encode %.3f s (%.3f ms/frame), decode %.3f s (%.3f ms/frame)\n",
           t_enc, 1000.0 * t_enc / n, t_dec, 1000.0 * t_dec / n);
    printf("written : %s (%d bytes), %s (%d samples)\n",
           c2_path, NB * n, raw_path, NS * n);
    return 0;
}
