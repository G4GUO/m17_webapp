//
// cm108_io.h
//
// The two things cm108.cpp needs from the outside world, and nothing else:
// a duplex PCM at 48 kHz carrying float mono, and a GPIO pin on the codec.
//
// They are separate translation units (cm108_alsa.cpp, cm108_hid.cpp) so that
// tests/cm108_test.cpp can supply its own fakes and exercise the whole back end
// - keying order, block sizes, symbol recovery - with no sound card and no
// soldering iron.  Same reasoning as the pty in tests/feather_test.cpp.
//
#ifndef M17WEB_CM108_IO_H
#define M17WEB_CM108_IO_H

#include <stdbool.h>
#include <stddef.h>

typedef struct cm108_pcm cm108_pcm;

// capture = 0 playback, 1 capture.  Both are opened at `rate`, mono in float;
// the layer does the S16 conversion and the channel duplication.
cm108_pcm *cm108_pcm_open(const char *dev, unsigned rate, int capture,
                          char *err, int errlen);
void cm108_pcm_close(cm108_pcm *p);

// Frames written / read, or -1.  A short write is a dropped block, not an error.
int cm108_pcm_write(cm108_pcm *p, const float *mono, int frames);
int cm108_pcm_read(cm108_pcm *p, float *mono, int frames);

// Frames waiting in a capture stream (used to spot a stalled receive loop).
int cm108_pcm_pending(cm108_pcm *p);

// Throw away whatever is buffered and restart the stream.  Used either side of
// a transmission: capture has been filling while the transmitter was keyed.
void cm108_pcm_drop(cm108_pcm *p);

const char *cm108_pcm_name(cm108_pcm *p);
unsigned    cm108_pcm_underruns(void);
unsigned    cm108_pcm_overruns(void);

// GPIO.  path == NULL means find a C-Media device; returns NULL if there is
// none.  gpio() drives `pin` (1..8) high or low and returns 0 on success.
void *cm108_hid_open(const char *path, char *err, int errlen);
void  cm108_hid_close(void *h);
int   cm108_hid_gpio(void *h, int pin, int on);
const char *cm108_hid_name(void *h);
unsigned    cm108_hid_writes(void);

#endif // M17WEB_CM108_IO_H
