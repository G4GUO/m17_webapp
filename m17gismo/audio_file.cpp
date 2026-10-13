// audio_file.cpp
//
// Raw 8 kHz mono s16le file back end.  Used for headless testing of the whole
// voice path (codec2 encode/decode, M17 framing) without sound hardware:
//
//     ./m17web -r sim -a file:in.raw:out.raw
//
// Microphone samples are read sequentially from the input file, restarting at
// the beginning each time the transmitter is keyed (that is what a "new
// transmission" means here).  Once the input runs out we feed silence so
// timing keeps working.  Decoded audio is appended to the output file: the M17
// code opens and closes the audio paths per transmission, so truncating on
// every open would throw away everything captured before the last PTT cycle.
//
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "m17defines.h"
#include "audio.h"

static char m_in_path[512];
static char m_out_path[512];
static FILE *m_in;
static FILE *m_out;
static int  m_out_opened;      // 1 once the header has been created
static bool m_eof;
static long m_read;
static long m_written;

void audio_file_set(const char *in_path, const char *out_path) {
    if (in_path)  { snprintf(m_in_path,  sizeof(m_in_path),  "%s", in_path); }
    if (out_path) { snprintf(m_out_path, sizeof(m_out_path), "%s", out_path); }
}

void audio_file_mic_open(void) {
#ifdef M17WEB_DIAG
    static int n = 0;
    fprintf(stdout, "diag-file: mic_open call %d -> %s\n", ++n, m_in_path[0] ? m_in_path : "(none)");
    fflush(stdout);
#endif
    if (m_in) { fclose(m_in); m_in = NULL; }
    m_eof = false;
    m_read = 0;
    if (m_in_path[0]) {
        m_in = fopen(m_in_path, "rb");
        if (!m_in) fprintf(stderr, "m17web: cannot open audio input %s\n", m_in_path);
    }
}

void audio_file_mic_close(void) {
    if (m_in) { fclose(m_in); m_in = NULL; }
}

void audio_file_spk_open(void) {
#ifdef M17WEB_DIAG
    static int n = 0;
    fprintf(stdout, "diag-file: spk_open call %d -> %s\n", ++n, m_out_path[0] ? m_out_path : "(none)");
    fflush(stdout);
#endif
    if (m_out) { fclose(m_out); m_out = NULL; }
    if (m_out_path[0]) {
        // "wb" only the first time: later re-opens append, so audio captured in
        // earlier transmit cycles survives
        m_out = fopen(m_out_path, m_out_opened ? "ab" : "wb");
        if (!m_out) fprintf(stderr, "m17web: cannot open audio output %s\n", m_out_path);
        else m_out_opened = 1;
    }
}

void audio_file_spk_close(void) {
    if (m_out) { fflush(m_out); fclose(m_out); m_out = NULL; }
}

void audio_file_input(int16_t *s, int len) {
    memset(s, 0, sizeof(int16_t) * len);
    if (!m_in || m_eof) return;
    size_t got = fread(s, sizeof(int16_t), (size_t)len, m_in);
    m_read += (long)got;
    if (got < (size_t)len) {
        if (feof(m_in)) m_eof = true;
    }
}

void audio_file_output(int16_t *s, int len) {
#ifdef M17WEB_DIAG
    static int n = 0;
    if ((n++ % 50) == 0) {
        fprintf(stdout, "diag-file: output m_out=%s written=%ld len=%d\n",
                m_out ? "open" : "NULL", m_written, len);
        fflush(stdout);
    }
#endif
    if (!m_out) return;
    size_t n2 = fwrite(s, sizeof(int16_t), (size_t)len, m_out);
    m_written += (long)n2;
}

bool audio_file_eof(void) { return m_eof; }
long audio_file_frames_read(void) { return m_read; }
long audio_file_frames_written(void) { return m_written; }
