// audio.cpp
//
// Audio dispatcher: the M17 code calls audio_input()/audio_output(); this
// module decides which of the compiled back ends actually carries the audio.
//
#include <stdio.h>
#include <string.h>
#include <math.h>

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
    if (strcasecmp(name, "pulse") == 0 || strcasecmp(name, "local") == 0) return AUDIO_PULSE;
    if (strcasecmp(name, "browser") == 0 || strcasecmp(name, "web") == 0) return AUDIO_BROWSER;
    if (strcasecmp(name, "file") == 0) return AUDIO_FILE;
    if (strcasecmp(name, "both") == 0) return AUDIO_BOTH;
    if (strcasecmp(name, "none") == 0) return AUDIO_NONE;
    return AUDIO_NONE;
}

const char *audio_backend_name(AudioBackend b) {
    switch (b) {
        case AUDIO_PULSE:   return "pulse";
        case AUDIO_BROWSER: return "browser";
        case AUDIO_FILE:    return "file";
        case AUDIO_BOTH:    return "both";
        default:            return "none";
    }
}

AudioBackend audio_get_backend(void) { return m_backend; }

void audio_backend_status(char *out, int len) {
    snprintf(out, len, "%s", audio_backend_name(m_backend));
}

// ---------------------------------------------------------------------------
void audio_mic_open(void) {
#ifdef HAVE_PULSE
    if (want_pulse(m_backend)) audio_pulse_mic_open();
#endif
    if (want_browser(m_backend)) audio_web_mic_open();
    if (want_file(m_backend))    audio_file_mic_open();
    m_mic_open = true;
    m_pending = AUDIO_NONE;
}

void audio_mic_close(void) {
#ifdef HAVE_PULSE
    audio_pulse_mic_close();
#endif
    audio_web_mic_close();
    audio_file_mic_close();
    m_mic_open = false;
    if (m_pending != AUDIO_NONE) { m_backend = m_pending; m_pending = AUDIO_NONE; }
}

void audio_spk_open(void) {
#ifdef HAVE_PULSE
    if (want_pulse(m_backend)) audio_pulse_spk_open();
#endif
    if (want_browser(m_backend)) audio_web_spk_open();
    if (want_file(m_backend))    audio_file_spk_open();
    m_spk_open = true;
    m_pending = AUDIO_NONE;
}

void audio_spk_close(void) {
#ifdef HAVE_PULSE
    audio_pulse_spk_close();
#endif
    audio_web_spk_close();
    audio_file_spk_close();
    m_spk_open = false;
    if (m_pending != AUDIO_NONE) { m_backend = m_pending; m_pending = AUDIO_NONE; }
}

// ---------------------------------------------------------------------------
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
    memset(s, 0, sizeof(int16_t) * len);
    if (!m_mic_open) return;

    if (want_browser(m_backend)) {
        // Preferred source when a browser is streaming; falls through to
        // PulseAudio/file when nobody is.
        if (audio_web_input(s, len) > 0) {
            level_update(&m_mic_level, s, len);
            return;
        }
    }
    if (want_file(m_backend)) {
        audio_file_input(s, len);
        level_update(&m_mic_level, s, len);
        return;
    }
#ifdef HAVE_PULSE
    if (want_pulse(m_backend)) {
        audio_pulse_input(s, len);
        level_update(&m_mic_level, s, len);
        return;
    }
#endif
    m_mic_level = 0;
}

void audio_output(int16_t *s, int len) {
    if (len <= 0) return;
    level_update(&m_spk_level, s, len);
    if (!m_spk_open) return;
#ifdef HAVE_PULSE
    if (want_pulse(m_backend)) audio_pulse_output(s, len);
#endif
    if (want_browser(m_backend)) audio_web_output(s, len);
    if (want_file(m_backend))    audio_file_output(s, len);
}

void audio_input_flush(void) {
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
