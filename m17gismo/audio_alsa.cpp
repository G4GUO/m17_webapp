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
#include <unistd.h>
#include <pthread.h>
#include <time.h>

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
static long m_overruns;     // capture overruns: microphone audio the card discarded
static long m_starved;      // capture samples lost (ring overflow, or padded silence)

//
// Capture runs on its own thread into this ring.  The transmit loop only comes
// back for more audio every 40 ms - and can be away longer than that while the
// radio pushes a block - so reading the card directly from that loop lets its
// ring overrun, and every overrun silently throws away microphone audio.  A
// thread that drains the card continuously and a ring that holds a few seconds
// means the card never sees the application's timing at all.
//
#define CAP_RING  16000     // 2 s of 8 kHz audio
static int16_t m_cap_ring[CAP_RING];
static int m_cap_head, m_cap_tail, m_cap_count;
static pthread_mutex_t m_cap_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  m_cap_cv  = PTHREAD_COND_INITIALIZER;
static pthread_t m_cap_thread;
static volatile bool m_cap_run;

const char *audio_alsa_error(void) { return m_error; }
long audio_alsa_overruns(void) { return m_overruns; }
long audio_alsa_starved(void)  { return m_starved; }
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
    // A restart can lose a race with the previous instance releasing the card,
    // and a close/open cycle in the radio loop can be a moment too early: retry
    // while the device reports itself busy rather than surfacing a stale error.
    int rc = 0;
    for (int attempt = 0; attempt < 6; attempt++) {
        rc = snd_pcm_open(pcm, dev, dir, 0);
        if (rc >= 0) break;
        if (rc != -EBUSY && rc != -EAGAIN) break;
        usleep(250000);
    }
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
    // Playback gets ~160 ms of headroom; capture gets twice that, because a slow
    // moment in the transmit loop overflows the card's capture ring and the
    // samples in it are gone - that is dropped transmit audio, not just latency.
    snd_pcm_uframes_t period = 160;
    snd_pcm_hw_params_set_period_size_near(*pcm, hw, &period, 0);
    unsigned int periods = (dir == SND_PCM_STREAM_CAPTURE) ? 16 : 8;
    snd_pcm_hw_params_set_periods_near(*pcm, hw, &periods, 0);
    if ((rc = snd_pcm_hw_params(*pcm, hw)) < 0) {
        snprintf(m_error, sizeof(m_error), "%s: %s", what, snd_strerror(rc));
        snd_pcm_close(*pcm);
        *pcm = NULL;
        return false;
    }
    snd_pcm_prepare(*pcm);
#ifdef M17WEB_DIAG
    {
        unsigned int got_rate = 0, got_ch = 0;
        snd_pcm_uframes_t got_period = 0, got_buf = 0;
        snd_pcm_hw_params_get_rate(hw, &got_rate, 0);
        snd_pcm_hw_params_get_channels(hw, &got_ch);
        snd_pcm_hw_params_get_period_size(hw, &got_period, 0);
        snd_pcm_hw_params_get_buffer_size(hw, &got_buf);
        fprintf(stdout, "diag-cap: %s %s -> rate=%u ch=%u period=%lu buffer=%lu\n",
                what, dev, got_rate, got_ch, (unsigned long)got_period, (unsigned long)got_buf);
        fflush(stdout);
    }
#endif
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

static void *cap_drain_thread(void *arg) {
    int16_t buf[160];
    long produced = 0;
    struct timespec tstart;
    clock_gettime(CLOCK_MONOTONIC, &tstart);
    while (m_cap_run) {
        snd_pcm_sframes_t n = snd_pcm_readi(m_in, buf, 160);
#ifdef M17WEB_DIAG
        static int nr = 0;
        if (n <= 0 || (nr++ % 100) == 0) {
            struct timespec tn;
            clock_gettime(CLOCK_MONOTONIC, &tn);
            double secs = (tn.tv_sec - tstart.tv_sec) + (tn.tv_nsec - tstart.tv_nsec) / 1e9;
            fprintf(stdout, "diag-cap: readi=%ld produced=%ld in %.2fs = %.0f samples/s ring=%d\n",
                    (long)n, produced, secs, secs > 0 ? produced / secs : 0.0, m_cap_count);
            fflush(stdout);
        }
#endif
        produced += (n > 0) ? n : 0;
        if (n == -EPIPE) {
            // Should not happen with this thread draining continuously; keep the
            // counter honest if it does.
            m_overruns++;
            snd_pcm_prepare(m_in);
            continue;
        }
        if (n == -EAGAIN) { usleep(2000); continue; }
        if (n <= 0) { usleep(5000); continue; }
        pthread_mutex_lock(&m_cap_mtx);
        for (int i = 0; i < n; i++) {
            if (m_cap_count >= CAP_RING) {
                // Nobody has read for two seconds: keep the newest audio and
                // count what had to go.
                m_cap_tail = (m_cap_tail + 1) % CAP_RING;
                m_cap_count--;
                m_starved++;
            }
            m_cap_ring[m_cap_head] = buf[i];
            m_cap_head = (m_cap_head + 1) % CAP_RING;
            m_cap_count++;
        }
        pthread_cond_broadcast(&m_cap_cv);
        pthread_mutex_unlock(&m_cap_mtx);
    }
    return arg;
}

void audio_alsa_mic_open(void) {
    char dev[128];
    resolve_device(dev, sizeof(dev));
    if (open_pcm(&m_in, dev, SND_PCM_STREAM_CAPTURE, "microphone")) {
        snprintf(m_used_in, sizeof(m_used_in), "%s", dev);
        pthread_mutex_lock(&m_cap_mtx);
        m_cap_head = m_cap_tail = m_cap_count = 0;
        pthread_mutex_unlock(&m_cap_mtx);
        m_cap_run = true;
#ifdef M17WEB_DIAG
        fprintf(stdout, "diag-cap: opening capture thread on %s\n", dev);
        fflush(stdout);
#endif
        if (pthread_create(&m_cap_thread, NULL, cap_drain_thread, NULL) != 0) {
            m_cap_run = false;
            snprintf(m_error, sizeof(m_error), "microphone (%s): cannot start capture thread", dev);
            snd_pcm_close(m_in);
            m_in = NULL;
        }
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

void audio_alsa_mic_close(void) {
#ifdef M17WEB_DIAG
    fprintf(stdout, "diag-cap: mic_close (run=%d ring=%d starved=%ld)\n",
            m_cap_run ? 1 : 0, m_cap_count, m_starved);
    fflush(stdout);
#endif
    if (m_cap_run) {
        m_cap_run = false;
        pthread_cond_broadcast(&m_cap_cv);
        pthread_join(m_cap_thread, NULL);
    }
    pthread_mutex_lock(&m_cap_mtx);
    m_cap_head = m_cap_tail = m_cap_count = 0;
    pthread_mutex_unlock(&m_cap_mtx);
    close_pcm(&m_in);
}
void audio_alsa_spk_close(void)  { close_pcm(&m_out); }

bool audio_alsa_ready(void) { return (m_in != NULL) || (m_out != NULL); }

void audio_alsa_input(int16_t *s, int len) {
    if (len <= 0) return;
    if (!m_in && !m_cap_run) { memset(s, 0, sizeof(int16_t) * (size_t)len); return; }

    struct timespec t0, t1, ts;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int got = 0;
    while (got < len) {
        pthread_mutex_lock(&m_cap_mtx);
        while (m_cap_count > 0 && got < len) {
            s[got++] = m_cap_ring[m_cap_tail];
            m_cap_tail = (m_cap_tail + 1) % CAP_RING;
            m_cap_count--;
        }
        if (got < len && m_cap_run) {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&m_cap_cv, &m_cap_mtx, &ts);
            pthread_mutex_unlock(&m_cap_mtx);
            // Keep the transmit loop pacing even if the capture side has gone
            // quiet: without this it would spin.
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long waited = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
            if (waited > 200) break;
            continue;
        }
        bool running = m_cap_run;
        pthread_mutex_unlock(&m_cap_mtx);
        if (!running) break;
    }
    if (got < len) {
        m_starved += len - got;
        memset(s + got, 0, sizeof(int16_t) * (size_t)(len - got));
    }
#ifdef M17WEB_DIAG
    {
        static int ni = 0;
        static struct timespec tfirst;
        struct timespec tn;
        if (ni == 0) clock_gettime(CLOCK_MONOTONIC, &tfirst);
        if ((ni++ % 50) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &tn);
            double secs = (tn.tv_sec - tfirst.tv_sec) + (tn.tv_nsec - tfirst.tv_nsec) / 1e9;
            long consumed = (long)ni * 160;
            fprintf(stdout, "diag-cap: in len=%d got=%d ring=%d starved=%ld consumed=%ld in %.2fs"
                            " = %.0f samples/s\n", len, got, m_cap_count, m_starved, consumed,
                    secs, secs > 0 ? consumed / secs : 0.0);
        }
        fflush(stdout);
    }
#endif
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
    pthread_mutex_lock(&m_cap_mtx);
    // Throw away everything the card delivered before the transmitter was
    // ready: that is audio from before the over, and keeping it would make the
    // whole transmission lag behind the microphone by however long it took the
    // radio to start.  It is a deliberate flush, so it is not a drop.
    m_cap_head = m_cap_tail = m_cap_count = 0;
    m_starved = 0;
    pthread_mutex_unlock(&m_cap_mtx);
}
