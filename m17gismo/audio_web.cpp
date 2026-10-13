// audio_web.cpp
//
// Browser audio back end.  Microphone PCM arrives from the WebSocket client
// (see webui.cpp) and is buffered here; decoded receive audio is handed to
// webui_audio_to_browser() which frames it for the connected browsers.
//
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#include "m17defines.h"
#include "audio.h"

#define MIC_RING    (8000 * 2)      // 2 seconds of 8 kHz audio
#define MIC_TIMEOUT_MS 400

static pthread_mutex_t m_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  m_cv  = PTHREAD_COND_INITIALIZER;

static int16_t m_ring[MIC_RING];
static int m_head, m_tail, m_count;      // ring buffer of mic samples
static bool m_mic_active;
static bool m_spk_active;
static long m_mic_frames;                // samples pushed by the browser
static long m_spk_frames;                // samples sent to the browser

// ---------------------------------------------------------------------------
void audio_web_mic_open(void) {
    pthread_mutex_lock(&m_mtx);
    m_head = m_tail = m_count = 0;
    m_mic_active = true;
    m_mic_frames = 0;
    pthread_mutex_unlock(&m_mtx);
}

void audio_web_mic_close(void) {
    pthread_mutex_lock(&m_mtx);
    m_mic_active = false;
    m_head = m_tail = m_count = 0;
    pthread_mutex_unlock(&m_mtx);
}

void audio_web_spk_open(void) {
    pthread_mutex_lock(&m_mtx);
    m_spk_active = true;
    m_spk_frames = 0;
    pthread_mutex_unlock(&m_mtx);
}

void audio_web_spk_close(void) {
    pthread_mutex_lock(&m_mtx);
    m_spk_active = false;
    pthread_mutex_unlock(&m_mtx);
}

bool audio_web_active(void) {
    bool r;
    pthread_mutex_lock(&m_mtx);
    r = m_mic_active || m_spk_active;
    pthread_mutex_unlock(&m_mtx);
    return r;
}

// ---------------------------------------------------------------------------
// Called from the WebSocket thread with microphone audio
void audio_web_mic_push(const int16_t *s, int len) {
    if (len <= 0) return;
    pthread_mutex_lock(&m_mtx);
    if (!m_mic_active) {
        pthread_mutex_unlock(&m_mtx);
        return;
    }
    for (int i = 0; i < len; i++) {
        if (m_count >= MIC_RING) {
            // Overrun: drop the oldest sample so we stay real time
            m_tail = (m_tail + 1) % MIC_RING;
            m_count--;
        }
        m_ring[m_head] = s[i];
        m_head = (m_head + 1) % MIC_RING;
        m_count++;
    }
    m_mic_frames += len;
    pthread_cond_signal(&m_cv);
    pthread_mutex_unlock(&m_mtx);
}

int audio_web_mic_available(void) {
    int n;
    pthread_mutex_lock(&m_mtx);
    n = m_count;
    pthread_mutex_unlock(&m_mtx);
    return n;
}

// Blocks until len samples are available (or the timeout expires)
int audio_web_input(int16_t *s, int len) {
    int got = 0;
    struct timespec ts;
    // The decoder needs whole 160 sample codec2 frames, so we wait for the
    // complete block; only a genuinely starved stream gets silence padding.
    long deadline_ms = MIC_TIMEOUT_MS;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&m_mtx);
    while (got < len) {
        if (m_count > 0) {
            s[got++] = m_ring[m_tail];
            m_tail = (m_tail + 1) % MIC_RING;
            m_count--;
            continue;
        }
        if (!m_mic_active) break;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += MIC_TIMEOUT_MS * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        // NOTE: pthread_cond_timedwait returns ETIMEDOUT, it does not set errno
        int rc = pthread_cond_timedwait(&m_cv, &m_mtx, &ts);
        if (rc != 0) {
            // No more audio coming: give up on this block if we have been
            // waiting long enough overall, otherwise keep trying for the rest.
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long waited = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
            if (waited >= deadline_ms * 3) break;
        }
    }
    pthread_mutex_unlock(&m_mtx);
    if (got < len) memset(&s[got], 0, sizeof(int16_t) * (len - got));
    return got;
}

void audio_web_output(int16_t *s, int len) {
    if (len <= 0) return;
    pthread_mutex_lock(&m_mtx);
    bool active = m_spk_active;
    m_spk_frames += len;
    pthread_mutex_unlock(&m_mtx);
    if (active) webui_audio_to_browser(s, len);
}

long audio_web_mic_frames(void) {
    long n;
    pthread_mutex_lock(&m_mtx);
    n = m_mic_frames;
    pthread_mutex_unlock(&m_mtx);
    return n;
}
