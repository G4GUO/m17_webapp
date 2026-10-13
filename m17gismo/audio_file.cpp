// audio_file.cpp
//
// Raw 8 kHz mono s16le file back end.  Used for headless testing of the whole
// voice path (codec2 encode/decode, M17 framing) without sound hardware:
//
//     ./m17web -r sim -a file:in.raw:out.raw ...
//
// Microphone samples are read sequentially from the input file; once it runs
// out we feed silence so timing keeps working.  Decoded audio is appended to
// the output file.
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
static bool m_eof;
static long m_read;
static long m_written;

void audio_file_set(const char *in_path, const char *out_path) {
    if (in_path)  { snprintf(m_in_path,  sizeof(m_in_path),  "%s", in_path); }
    if (out_path) { snprintf(m_out_path, sizeof(m_out_path), "%s", out_path); }
}

void audio_file_mic_open(void) {
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
    if (m_out) { fclose(m_out); m_out = NULL; }
    m_written = 0;
    if (m_out_path[0]) {
        m_out = fopen(m_out_path, "wb");
        if (!m_out) fprintf(stderr, "m17web: cannot open audio output %s\n", m_out_path);
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
    if (!m_out) return;
    size_t n = fwrite(s, sizeof(int16_t), (size_t)len, m_out);
    m_written += (long)n;
}

bool audio_file_eof(void) { return m_eof; }
long audio_file_frames_read(void) { return m_read; }
long audio_file_frames_written(void) { return m_written; }
