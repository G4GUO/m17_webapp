// audio_alsa.cpp
//
// ALSA back end: talks to the sound card directly, with no PulseAudio in the
// path.  On this Pi that matters, because Debian's ALSA "default" PCM is routed
// *through* PulseAudio, and PulseAudio can refuse to run a card (it leaves the
// sink suspended with the ALSA device closed while a stream sits attached).
//
// Device selection, in order:
//   1. the device set with "audio alsa <device>" (web UI / config.txt / command)
//   2. the M17WEB_ALSA_DEVICE environment variable
//   3. the first USB/PnP audio card found (that is usually the one you plugged
//      in, and HDMI is never it), as plughw:<card>,0 which handles rate
//      conversion for 8 kHz mono
//   4. "sysdefault" if no USB card is present
//
// Example overrides: "audio alsa plughw:3,0", "audio alsa hw:CARD=Device,DEV=0"
//
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

#include <alsa/asoundlib.h>

#include "m17defines.h"
#include "audio.h"

#define ALSA_RATE 8000

static snd_pcm_t *m_out;
static snd_pcm_t *m_in;
static char m_requested[128];
static char m_used_in[128];
static char m_used_out[128];
static char m_error[160];

const char *audio_alsa_error(void) { return m_error; }
const char *audio_alsa_device(void) { return m_used_in[0] ? m_used_in : m_requested; }

void audio_alsa_set_device(const char *dev) {
    snprintf(m_requested, sizeof(m_requested), "%s", dev ? dev : "");
}

// Pick something sane when the user has not said which device to use.
static void resolve_device(char *out, size_t n) {
    if (m_requested[0]) { snprintf(out, n, "%s", m_requested); return; }
    const char *env = getenv("M17WEB_ALSA_DEVICE");
    if (env && env[0]) { snprintf(out, n, "%s", env); return; }

    char best[64] = "";
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char *nm = NULL;
        if (snd_card_get_longname(card, &nm) == 0 && nm) {
            if (strcasestr(nm, "USB") || strcasestr(nm, "PnP"))
                snprintf(best, sizeof(best), "plughw:%d,0", card);
            free(nm);
        }
    }
    snprintf(out, n, "%s", best[0] ? best : "sysdefault");
}

static bool open_pcm(snd_pcm_t **pcm, const char *dev, snd_pcm_stream_t dir, const char *what) {
    int rc = snd_pcm_open(pcm, dev, dir, 0);
    if (rc < 0) {
        snprintf(m_error, sizeof(m_error), "%s (%s): %s", what, dev, snd_strerror(rc));
        return false;
    }
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    if ((rc = snd_pcm_hw_params_any(*pcm, hw)) < 0 ||
        (rc = snd_pcm_hw_params_set_access(*pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (rc = snd_pcm_hw_params_set_format(*pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (rc = snd_pcm_hw_params_set_channels(*pcm, hw, 1)) < 0) {
        snprintf(m_error, sizeof(m_error), "%s: parameter setup: %s", what, snd_strerror(rc));
        snd_pcm_close(*pcm);
        *pcm = NULL;
        return false;
    }
    unsigned int rate = ALSA_RATE;
    if ((rc = snd_pcm_hw_params_set_rate_near(*pcm, hw, &rate, 0)) < 0) {
        snprintf(m_error, sizeof(m_error), "%s: %u Hz not usable: %s", what, ALSA_RATE, snd_strerror(rc));
        snd_pcm_close(*pcm);
        *pcm = NULL;
        return false;
    }
    // ~20 ms of audio per period (one codec2 3200 frame) keeps the pacing right.
    // Eight periods give ~160 ms of headroom, which matters because the transmit
    // loop also spends time encoding and decoding between writes.
    snd_pcm_uframes_t period = 160;
    snd_pcm_hw_params_set_period_size_near(*pcm, hw, &period, 0);
    unsigned int periods = 8;
    snd_pcm_hw_params_set_periods_near(*pcm, hw, &periods, 0);
    if ((rc = snd_pcm_hw_params(*pcm, hw)) < 0) {
        snprintf(m_error, sizeof(m_error), "%s: %s", what, snd_strerror(rc));
        snd_pcm_close(*pcm);
        *pcm = NULL;
        return false;
    }
    snd_pcm_prepare(*pcm);
    m_error[0] = 0;
    return true;
}

static void close_pcm(snd_pcm_t **pcm) {
    if (*pcm) {
        snd_pcm_drain(*pcm);
        snd_pcm_close(*pcm);
        *pcm = NULL;
    }
}

void audio_alsa_mic_open(void) {
    char dev[128];
    resolve_device(dev, sizeof(dev));
    if (open_pcm(&m_in, dev, SND_PCM_STREAM_CAPTURE, "microphone")) {
        snprintf(m_used_in, sizeof(m_used_in), "%s", dev);
    } else {
        printf("Audio Input failed: %s\n", m_error);
    }
}

void audio_alsa_spk_open(void) {
    char dev[128];
    resolve_device(dev, sizeof(dev));
    if (open_pcm(&m_out, dev, SND_PCM_STREAM_PLAYBACK, "speaker")) {
        snprintf(m_used_out, sizeof(m_used_out), "%s", dev);
        // Prefill with silence: starting playback on an empty buffer guarantees
        // an immediate underrun (XRUN) on the first write.
        int16_t silence[160];
        memset(silence, 0, sizeof(silence));
        for (int i = 0; i < 4; i++) snd_pcm_writei(m_out, silence, 160);
        snd_pcm_start(m_out);
    } else {
        printf("Audio Output failed: %s\n", m_error);
    }
}

void audio_alsa_mic_close(void)  { close_pcm(&m_in); }
void audio_alsa_spk_close(void)  { close_pcm(&m_out); }

bool audio_alsa_ready(void) { return (m_in != NULL) || (m_out != NULL); }

void audio_alsa_input(int16_t *s, int len) {
    if (!m_in) return;
    int got = 0;
    while (got < len) {
        snd_pcm_sframes_t n = snd_pcm_readi(m_in, s + got, (snd_pcm_uframes_t)(len - got));
        if (n > 0) { got += (int)n; continue; }
        if (n == -EPIPE) { snd_pcm_prepare(m_in); continue; }   // overrun
        if (n == -EAGAIN) { continue; }
        break;
    }
    if (got < len) memset(s + got, 0, sizeof(int16_t) * (size_t)(len - got));
}

void audio_alsa_output(int16_t *s, int len) {
    if (!m_out) return;
    int done = 0;
    while (done < len) {
        snd_pcm_sframes_t n = snd_pcm_writei(m_out, s + done, (snd_pcm_uframes_t)(len - done));
        if (n > 0) { done += (int)n; continue; }
        if (n == -EPIPE) { snd_pcm_prepare(m_out); continue; }  // underrun
        if (n == -EAGAIN) { continue; }
        break;
    }
}

void audio_alsa_flush(void) {
    if (m_out) snd_pcm_drain(m_out);
    if (m_in)  snd_pcm_drop(m_in);
}
