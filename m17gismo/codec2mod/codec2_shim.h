// codec2mod/codec2_shim.h
//
// Adapter for the modified Codec2 (M17-Project/Codec2-mod), vendored in this
// directory.  That fork implements *only* the 3200 bps mode - 8 bytes (160
// bits) per 20 ms frame of 160 samples at 8 kHz - which is exactly the mode M17
// uses, and it uses static, caller-provided codec state instead of
// codec2_create()'s dynamic allocation.
//
// This shim presents the subset of the upstream libcodec2 API that m17web
// actually uses, so the rest of the project is unchanged:
//
//     codec2_create(CODEC2_MODE_3200)   codec2_samples_per_frame()
//     codec2_encode()                   codec2_decode()
//
// Built with CODEC2_BACKEND=mod in the makefile.  Decoded audio is not
// guaranteed bit-identical to upstream Codec2 (only the encoded bitstream is),
// so the decoder can be swapped without breaking interoperability with other
// M17 stations.
//
#ifndef CODEC2_SHIM_H
#define CODEC2_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CODEC2_MODE_3200          0
#define CODEC2M_BYTES_PER_FRAME   8
#define CODEC2M_SAMPLES_PER_FRAME 160
#define CODEC2M_MAX_CODECS        4      // fixed pool: no dynamic allocation

// Opaque, but deliberately tagged CODEC2 so existing code that writes
// "struct CODEC2 *" keeps compiling.
typedef struct CODEC2 CODEC2;

CODEC2 *codec2m_create(int mode);
void    codec2m_destroy(CODEC2 *c);
int     codec2m_samples_per_frame(CODEC2 *c);
int     codec2m_bits_per_frame(CODEC2 *c);
int     codec2m_bytes_per_frame(CODEC2 *c);
void    codec2m_encode(CODEC2 *c, uint8_t *bits, int16_t *speech);
void    codec2m_decode(CODEC2 *c, int16_t *speech, const uint8_t *bits);

#ifdef __cplusplus
}
#endif

#ifndef CODEC2_SHIM_INTERNAL
// Upstream spelling, so callers do not need to know which codec is linked in
#define codec2_create            codec2m_create
#define codec2_destroy           codec2m_destroy
#define codec2_samples_per_frame codec2m_samples_per_frame
#define codec2_bits_per_frame    codec2m_bits_per_frame
#define codec2_encode            codec2m_encode
#define codec2_decode            codec2m_decode
#endif

#endif // CODEC2_SHIM_H
