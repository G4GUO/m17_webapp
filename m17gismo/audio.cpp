// audio.cpp
//
// Audio dispatcher: the M17 code calls audio_input()/audio_output(); this
// module decides which of the compiled back ends actually carries the audio.
//
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "m17defines.h"
#include "audio.h"

static AudioBackend m_backend = AUDIO_BROWSER;
static bool m_mic_open;
static bool m_spk_open;
static AudioBackend m_pending = AUDIO_NONE;   // requested while open

static double m_mic_level;
static double m_spk_level;

// ---------------------------------------------------------------------------
static bool want_pulse(AudioBackend b) {
    return b == AUDIO_PULSE || b == AUDIO_BOTH;
}
static bool want_browser(AudioBackend b) {
    return b == AUDIO_BROWSER || b == AUDIO_BOTH;
}
static bool want_file(AudioBackend b) {
    return b == AUDIO_FILE;
}
static bool want_alsa(AudioBackend b) {
    return b == AUDIO_ALSA;
}

bool audio_set_backend(AudioBackend b) {
    if (b == AUDIO_NONE) {
        // Nothing to do: silence in, discard out
    }
    m_backend = b;
    if (m_mic_open || m_spk_open) {
        m_pending = b;   // applied on the next close
    }
    return true;
}

AudioBackend audio_parse_backend(const char *name) {
    if (!name) return AUDIO_NONE;
    if (strcasecmp(name, "alsa") == 0 || strcasecmp(name, "local") == 0) return AUDIO_ALSA;
    if (strcasecmp(name, "pulse") == 0) return AUDIO_PULSE;
    if (strcasecmp(name, "browser") == 0 || strcasecmp(name, "web") == 0) return AUDIO_BROWSER;
    if (strcasecmp(name, "file") == 0) return AUDIO_FILE;
    if (strcasecmp(name, "both") == 0) return AUDIO_BOTH;
    if (strcasecmp(name, "none") == 0) return AUDIO_NONE;
    return AUDIO_NONE;
}

const char *audio_backend_name(AudioBackend b) {
    switch (b) {
        case AUDIO_ALSA:    return "alsa";
        case AUDIO_PULSE:   return "pulse";
        case AUDIO_BROWSER: return "browser";
        case AUDIO_FILE:    return "file";
        case AUDIO_BOTH:    return "both";
        default:            return "none";
    }
}

AudioBackend audio_get_backend(void) { return m_backend; }

// A change was requested while the streams were open: it is applied the next
// time they are closed/opened (the M17 code does that per transmit/receive
// cycle), so the UI can say so and the radio loop can apply it promptly.
bool audio_backend_pending(void) { return m_pending != AUDIO_NONE; }

bool audio_have_pulse(void) {
#ifdef HAVE_PULSE
    return true;
#else
    return false;
#endif
}

bool audio_have_alsa(void) {
#ifdef HAVE_ALSA
    return true;
#else
    return false;
#endif
}

bool audio_have_browser_audio(void) { return true; }

bool audio_local_ready(void) {
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) return audio_alsa_ready();
#endif
#ifdef HAVE_PULSE
    if (want_pulse(m_backend)) return audio_pulse_ready();
#endif
    return false;
}

const char *audio_local_device(void) {
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) return audio_alsa_device();
#endif
    return "";
}

const char *audio_backend_error(void) {
#ifdef HAVE_ALSA
    if (want_alsa(m_backend) && audio_alsa_error()[0])
        return audio_alsa_error();
#endif
#ifdef HAVE_PULSE
    if (want_pulse(m_backend) && audio_pulse_error()[0])
        return audio_pulse_error();
#endif
    return "";
}

void audio_backend_status(char *out, int len) {
    snprintf(out, len, "%s", audio_backend_name(m_backend));
}

// ---------------------------------------------------------------------------
void audio_mic_open(void) {
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) audio_alsa_mic_open();
#endif
#ifdef HAVE_PULSE
    if (want_pulse(m_backend)) audio_pulse_mic_open();
#endif
    if (want_browser(m_backend)) audio_web_mic_open();
    if (want_file(m_backend))    audio_file_mic_open();
    m_mic_open = true;
    m_pending = AUDIO_NONE;
}

void audio_mic_close(void) {
#ifdef HAVE_ALSA
    audio_alsa_mic_close();
#endif
#ifdef HAVE_PULSE
    audio_pulse_mic_close();
#endif
    audio_web_mic_close();
    audio_file_mic_close();
    m_mic_open = false;
    if (m_pending != AUDIO_NONE) { m_backend = m_pending; m_pending = AUDIO_NONE; }
}

void audio_spk_open(void) {
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) audio_alsa_spk_open();
#endif
#ifdef HAVE_PULSE
    if (want_pulse(m_backend)) audio_pulse_spk_open();
#endif
    if (want_browser(m_backend)) audio_web_spk_open();
    if (want_file(m_backend))    audio_file_spk_open();
    m_spk_open = true;
    m_pending = AUDIO_NONE;
}

void audio_spk_close(void) {
#ifdef HAVE_ALSA
    audio_alsa_spk_close();
#endif
#ifdef HAVE_PULSE
    audio_pulse_spk_close();
#endif
    audio_web_spk_close();
    audio_file_spk_close();
    m_spk_open = false;
    if (m_pending != AUDIO_NONE) { m_backend = m_pending; m_pending = AUDIO_NONE; }
}

// ---------------------------------------------------------------------------
// The M17 transmit path expects one 20 ms codec frame per call, so hold the
// caller to real time.  A sound card that blocks already takes that long; the
// file/none paths (and a sound card that failed to open) return instantly and
// would otherwise spin the transmit loop at 100% CPU.
static void audio_pace(struct timespec t0, int len) {
    if (len <= 0) return;
    long want_us = (long)len * 1000000L / 8000;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long took_us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
    if (took_us < want_us) usleep((useconds_t)(want_us - took_us));
}

static void level_update(double *acc, const int16_t *s, int len) {
    if (len <= 0) return;
    double sum = 0;
    for (int i = 0; i < len; i++) {
        double v = s[i] / 32768.0;
        sum += v * v;
    }
    *acc = sqrt(sum / len);
    if (*acc > 1.0) *acc = 1.0;
}

void audio_input(int16_t *s, int len) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
#ifdef M17WEB_DIAG
    static int ni = 0;
    if ((ni++ % 200) == 0)
        fprintf(stdout, "diag-audio: in  len=%d mic_open=%d backend=%s\n", len, m_mic_open,
                audio_backend_name(m_backend)), fflush(stdout);
#endif
    memset(s, 0, sizeof(int16_t) * len);
    if (!m_mic_open) return;

    if (want_browser(m_backend)) {
        // Preferred source when a browser is streaming; falls through to
        // PulseAudio/file when nobody is.
        if (audio_web_input(s, len) > 0) {
            level_update(&m_mic_level, s, len);
            audio_pace(t0, len);
            return;
        }
    }
    if (want_file(m_backend)) {
        audio_file_input(s, len);
        level_update(&m_mic_level, s, len);
        audio_pace(t0, len);
        return;
    }
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) {
        audio_alsa_input(s, len);
        level_update(&m_mic_level, s, len);
        audio_pace(t0, len);
        return;
    }
#endif
#ifdef HAVE_PULSE
    if (want_pulse(m_backend)) {
        audio_pulse_input(s, len);
        level_update(&m_mic_level, s, len);
        audio_pace(t0, len);
        return;
    }
#endif
    m_mic_level = 0;
    audio_pace(t0, len);
}

void audio_output(int16_t *s, int len) {
#ifdef M17WEB_DIAG
    static int no = 0;
    if ((no++ % 50) == 0)
        fprintf(stdout, "diag-audio: out len=%d spk_open=%d backend=%s first=%d\n", len, m_spk_open,
                audio_backend_name(m_backend), (s && len > 0) ? s[0] : 0), fflush(stdout);
#endif
    if (len <= 0) return;
    level_update(&m_spk_level, s, len);
    if (!m_spk_open) return;
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) audio_alsa_output(s, len);
#endif
#ifdef HAVE_PULSE
    if (want_pulse(m_backend)) audio_pulse_output(s, len);
#endif
    if (want_browser(m_backend)) audio_web_output(s, len);
    if (want_file(m_backend))    audio_file_output(s, len);
}

void audio_input_flush(void) {
#ifdef HAVE_ALSA
    if (want_alsa(m_backend)) audio_alsa_flush();
#endif
#ifdef HAVE_PULSE
    audio_pulse_flush();
#endif
    if (want_file(m_backend)) audio_file_mic_close();
}

void audio_output_flush(void) {
#ifdef HAVE_PULSE
    audio_pulse_flush();
#endif
}

double audio_get_mic_level(void) { return m_mic_level; }
double audio_get_spk_level(void) { return m_spk_level; }
