// m17_mailbox.cpp
//
// The AI link as two directories.  See m17_mailbox.h.
//
// Why files rather than a socket: a client streaming microphone frames has to
// guess the rate the transmitter draws them at, and every guess is audible - too
// slow and the vocoder pads the middle of a word, too fast and the ring overruns
// and evicts audio the transmitter has not read.  A file is read on demand, so
// the rate is always right, and the over ends at end of file.
//
// The inbox side uses the recorder, so each over arrives as a complete WAV.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "m17defines.h"
#include "audio.h"
#include "recorder.h"
#include "fargan.h"
#include "m17_mailbox.h"

static bool m_on;
static char m_inbox[256];
static char m_outbox[256];

static bool m_recording;
static char m_rec_path[300];
static bool m_los_pending;           // sync was lost; the over ends after a hang
static long m_los_at;
#define MAILBOX_HANG_MS 3000        // a brief loss of sync is a pause, not the end

static int  m_state;                 // 0 idle, 1 keyed and playing, 2 draining
static char m_playing[300];
static struct timespec m_drain_at;

static long mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void make_dir(const char *path) {
    if (!path || !path[0]) return;
    mkdir(path, 0775);                     // the simple case
    // ...and the whole path, for a mailbox under a directory that does not exist
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' 2>/dev/null", path);
    if (system(cmd) != 0) { /* nothing more we can do; the poll will simply find
                               no files and the operator sees it in the log */ }
}

void m17_mailbox_set(const char *inbox, const char *outbox) {
    if (!inbox || !inbox[0]) {
        m_on = false;
        m_inbox[0] = 0;
        m_outbox[0] = 0;
        return;
    }
    snprintf(m_inbox, sizeof(m_inbox), "%s", inbox);
    snprintf(m_outbox, sizeof(m_outbox), "%s", (outbox && outbox[0]) ? outbox : inbox);
    make_dir(m_inbox);
    make_dir(m_outbox);
    m_on = true;
}

bool m17_mailbox_enabled(void) { return m_on; }
bool m17_mailbox_busy(void) { return m_state != 0; }

void m17_mailbox_aos(void) {
    if (!m_on || !m_inbox[0]) return;
    // Sync came back inside the hang time: this is one over, keep recording it.
    m_los_pending = false;
    if (m_recording) return;
    if (rec_active()) return;               // somebody is already recording
    snprintf(m_rec_path, sizeof(m_rec_path), "%s/over-%ld.wav", m_inbox, (long)time(NULL));
    // The neural receiver pushes its own 16 kHz output into a recording, and the
    // recorder ignores samples whose rate is not the one it opened with - so the
    // file must be opened at whatever the decoder is actually producing, or it
    // fills with a header and nothing else.
    int rate = fargan_rx_enabled() ? 16000 : 8000;
    if (rec_start(m_rec_path, rate)) {
        m_recording = true;
        printf("mailbox: receiving into %s\n", m_rec_path);
        fflush(stdout);
    }
}

void m17_mailbox_los(void) {
    if (!m_recording) return;
    // The receiver loses sync between words and across a pause.  Ending the file
    // on every loss turned one over into a handful of fragments, none of them long
    // enough to transcribe - so wait, and only finish if nothing comes back.
    m_los_pending = true;
    m_los_at = mono_ms();
}

// The next file to send: ".raw" only, since that is what the microphone source
// reads; a WAV header would be played as noise.  Oldest name first, so numbered
// replies go out in order.
static bool next_outbox_file(char *out, size_t len) {
    DIR *d = opendir(m_outbox);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || n < 5) continue;
        if (strcasecmp(e->d_name + n - 4, ".raw") != 0) continue;
        char cand[300];
        snprintf(cand, sizeof(cand), "%s/%s", m_outbox, e->d_name);
        if (!found || strcmp(cand, out) < 0) {
            snprintf(out, len, "%s", cand);
            found = true;
        }
    }
    closedir(d);
    return found;
}

void m17_mailbox_service(void) {
    if (!m_on) return;

    if (m_recording && m_los_pending && mono_ms() - m_los_at >= MAILBOX_HANG_MS) {
        rec_stop();
        m_recording = false;
        m_los_pending = false;
        printf("mailbox: over written to %s\n", m_rec_path);
        fflush(stdout);
    }

    if (m_state == 0) {
        char path[300];
        if (next_outbox_file(path, sizeof(path))) {
            audio_mic_file_set(path);
            snprintf(m_playing, sizeof(m_playing), "%s", path);
            m17_txrx_ptt_on();
            m_state = 1;
            printf("mailbox: sending %s\n", path);
            fflush(stdout);
        }
        return;
    }

    if (m_state == 1) {
        // The transmit loop unkeys itself at end of file; wait for that to settle
        // before dropping the clip, or it would be deleted mid-over.
        if (audio_mic_file_eof()) {
            clock_gettime(CLOCK_MONOTONIC, &m_drain_at);
            m_state = 2;
        }
        return;
    }

    long now = mono_ms();
    long at = m_drain_at.tv_sec * 1000L + m_drain_at.tv_nsec / 1000000L;
    if (now - at >= 400) {
        m17_txrx_ptt_off();
        audio_mic_file_set("");
        if (unlink(m_playing) == 0) {
            printf("mailbox: sent and removed %s\n", m_playing);
        } else {
            printf("mailbox: sent %s (could not remove it)\n", m_playing);
        }
        fflush(stdout);
        m_playing[0] = 0;
        m_state = 0;
    }
}
