//
// cm108_alsa.cpp
//
// Duplex 48 kHz through ALSA, float mono at this interface and S16 interleaved
// at the device.  Nothing clever on purpose: a sound card is the one part of
// this back end that must not add rate conversion, so the rate is requested
// exactly and the open fails if the device will not give it.
//
// `hw:` is what should be passed in.  `plughw:` and `default` may go through a
// resampler, and a fraction of a percent of rate error is a burst that decodes
// at 97% of the frames and fails on the rest.
//
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include "cm108_io.h"

struct cm108_pcm {
    snd_pcm_t *h;
    int        capture;
    unsigned   chans;
    char       name[64];
};

static unsigned g_underruns;
static unsigned g_overruns;

static void set_err(char *err, int errlen, const char *fmt, const char *arg)
{
    if (err && errlen > 0) snprintf(err, (size_t)errlen, fmt, arg ? arg : "");
}

cm108_pcm *cm108_pcm_open(const char *dev, unsigned rate, int capture,
                          char *err, int errlen)
{
    snd_pcm_t *h = NULL;
    int rc = snd_pcm_open(&h, dev, capture ? SND_PCM_STREAM_CAPTURE
                                           : SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        set_err(err, errlen, "%s", snd_strerror(rc));
        return NULL;
    }

    // The CM108 capture side is mono and the playback side is stereo; a device
    // that reports otherwise still works, so try both for capture.
    unsigned tries[2] = { capture ? 1u : 2u, capture ? 2u : 2u };
    int ok = 0;
    for (int t = 0; t < 2 && !ok; t++) {
        snd_pcm_hw_params_t *p = NULL;
        snd_pcm_hw_params_alloca(&p);
        unsigned chans = tries[t];
        unsigned r = rate;
        int dir = 0;

        if (snd_pcm_hw_params_any(h, p) < 0) break;
        if (snd_pcm_hw_params_set_access(h, p, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) break;
        if (snd_pcm_hw_params_set_format(h, p, SND_PCM_FORMAT_S16_LE) < 0) break;
        if (snd_pcm_hw_params_set_channels(h, p, chans) < 0) continue;
        if (snd_pcm_hw_params_set_rate_near(h, p, &r, &dir) < 0) break;
        if (r != rate) {
            set_err(err, errlen, "device will not do %s Hz - use hw: with a "
                                 "native 48 kHz device", "48000");
            break;
        }
        // 10 ms periods and a buffer long enough to ride out a scheduling
        // hiccup while a frame is being assembled: the transmit path writes in
        // 40 ms frames, not continuously.
        unsigned buffer_us = 150000, period_us = 10000;
        snd_pcm_hw_params_set_buffer_time_near(h, p, &buffer_us, &dir);
        snd_pcm_hw_params_set_period_time_near(h, p, &period_us, &dir);

        if (snd_pcm_hw_params(h, p) < 0) continue;
        snd_pcm_prepare(h);
        ok = 1;
        cm108_pcm *out = (cm108_pcm *)calloc(1, sizeof(*out));
        if (out == NULL) { snd_pcm_close(h); return NULL; }
        out->h = h;
        out->capture = capture;
        out->chans = chans;
        snprintf(out->name, sizeof(out->name), "%s%s", dev, capture ? " in" : " out");
        return out;
    }

    set_err(err, errlen, "%s", "no usable 48 kHz S16 configuration");
    snd_pcm_close(h);
    return NULL;
}

void cm108_pcm_close(cm108_pcm *p)
{
    if (p == NULL) return;
    snd_pcm_drop(p->h);         // never wait for a stream the radio has stopped
    snd_pcm_close(p->h);
    free(p);
}

// Recover from the two states a stream gets into when nobody feeds it: the
// buffer ran dry (EPIPE) or the device was suspended.
static int recover(cm108_pcm *p, int rc, unsigned *count)
{
    if (rc == -EPIPE) {
        if (count) (*count)++;
        snd_pcm_prepare(p->h);
        return 1;
    }
    if (rc == -ESTRPIPE) {
        while (snd_pcm_resume(p->h) == -EAGAIN) snd_pcm_wait(p->h, 100);
        snd_pcm_prepare(p->h);
        return 1;
    }
    return 0;
}

int cm108_pcm_write(cm108_pcm *p, const float *mono, int frames)
{
    if (p == NULL || mono == NULL || frames <= 0) return -1;
    if (frames > 8192) frames = 8192;

    int16_t buf[8192 * 2];
    for (int i = 0; i < frames; i++) {
        float v = mono[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        int16_t s = (int16_t)lrintf(v * 32767.0f);
        for (unsigned c = 0; c < p->chans; c++) buf[i * p->chans + c] = s;
    }

    int done = 0;
    for (int guard = 0; guard < 8 && done < frames; guard++) {
        int rc = snd_pcm_writei(p->h, buf + done * p->chans, frames - done);
        if (rc > 0) { done += rc; continue; }
        if (rc == -EAGAIN) { snd_pcm_wait(p->h, 100); continue; }
        if (!recover(p, rc, &g_underruns)) break;
    }
    return done;
}

int cm108_pcm_read(cm108_pcm *p, float *mono, int frames)
{
    if (p == NULL || mono == NULL || frames <= 0) return -1;
    if (frames > 8192) frames = 8192;

    static int16_t buf[8192 * 2];
    int done = 0;
    for (int guard = 0; guard < 8 && done < frames; guard++) {
        int rc = snd_pcm_readi(p->h, buf, frames - done);
        if (rc > 0) {
            for (int i = 0; i < rc; i++)
                mono[done + i] = (float)buf[i * p->chans] / 32768.0f;   // channel 0
            done += rc;
            continue;
        }
        if (rc == -EAGAIN) { snd_pcm_wait(p->h, 100); continue; }
        if (!recover(p, rc, &g_overruns)) break;
    }
    return done;
}

int cm108_pcm_pending(cm108_pcm *p)
{
    if (p == NULL) return 0;
    snd_pcm_sframes_t avail = snd_pcm_avail(p->h);
    return avail > 0 ? (int)avail : 0;
}

void cm108_pcm_drop(cm108_pcm *p)
{
    if (p == NULL) return;
    snd_pcm_drop(p->h);
    snd_pcm_prepare(p->h);
}

const char *cm108_pcm_name(cm108_pcm *p) { return p ? p->name : "none"; }
unsigned cm108_pcm_underruns(void) { return g_underruns; }
unsigned cm108_pcm_overruns(void)  { return g_overruns; }
