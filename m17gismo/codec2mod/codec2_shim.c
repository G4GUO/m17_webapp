// codec2mod/codec2_shim.c
//
// Maps the upstream libcodec2 API used by m17web onto the modified Codec2
// (M17-Project/Codec2-mod) vendored next to this file.
//
// The mod's own public symbols are renamed by the makefile
// (-Dcodec2_encode=codec2raw_encode, ...) so that a single binary can link
// both this codec and the stock libcodec2 - which is what the bitexact test
// does to compare them.
//
#define CODEC2_SHIM_INTERNAL 1
#include "codec2_shim.h"

#include <string.h>
#include <stdio.h>

#include "codec2_mod.h"        // the modified codec (-Icodec2mod/inc)

// Renamed entry points of the modified codec (see CODEC2_MOD_RENAME)
extern void codec2raw_init(codec2_t *c2);
extern void codec2raw_encoder_init(codec2_encoder_t *c2);
extern void codec2raw_decoder_init(codec2_decoder_t *c2);
extern void codec2raw_encode(codec2_encoder_t *c2, uint8_t *bits, const int16_t *speech);
extern void codec2raw_decode(codec2_decoder_t *c2, int16_t *speech, const uint8_t *bits);

struct CODEC2 {
    codec2_t state;
    int      in_use;
};

// Static pool: the mod is designed for static allocation, and m17web only ever
// needs one instance (an encoder and a decoder are both inside codec2_t).
static struct CODEC2 m_pool[CODEC2M_MAX_CODECS];

CODEC2 *codec2m_create(int mode) {
    if (mode != CODEC2_MODE_3200) return NULL;      // 3200 bps is all there is
    for (int i = 0; i < CODEC2M_MAX_CODECS; i++) {
        if (!m_pool[i].in_use) {
            memset(&m_pool[i].state, 0, sizeof(m_pool[i].state));
            codec2raw_init(&m_pool[i].state);       // encoder + decoder
            m_pool[i].in_use = 1;
            return &m_pool[i];
        }
    }
    return NULL;
}

void codec2m_destroy(CODEC2 *c) {
    if (c) c->in_use = 0;
}

int codec2m_samples_per_frame(CODEC2 *c) { (void)c; return CODEC2M_SAMPLES_PER_FRAME; }
int codec2m_bits_per_frame(CODEC2 *c)    { (void)c; return CODEC2M_BYTES_PER_FRAME * 8; }
int codec2m_bytes_per_frame(CODEC2 *c)   { (void)c; return CODEC2M_BYTES_PER_FRAME; }

void codec2m_encode(CODEC2 *c, uint8_t *bits, int16_t *speech) {
    if (!c || !bits || !speech) return;
    codec2raw_encode(&c->state.encoder, bits, speech);
}

void codec2m_decode(CODEC2 *c, int16_t *speech, const uint8_t *bits) {
    if (!c || !speech || !bits) return;
    codec2raw_decode(&c->state.decoder, speech, bits);
}
