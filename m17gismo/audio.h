// audio.h
//
// Audio routing for m17web.
//
// The M17 voice path always works with 8 kHz mono signed 16 bit PCM in blocks
// of codec2_samples_per_frame() samples (160 for CODEC2_MODE_3200).
//
// Where that audio comes from and goes to is selectable at run time:
//
//   pulse    local sound card via PulseAudio simple API
//   browser  microphone / speaker in the web browser, carried over WebSocket
//   file     raw 8 kHz s16le files (headless testing, no sound hardware)
//   both     browser in/out plus PulseAudio out
//   none     black hole (silence in, discard out)
//
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    AUDIO_NONE = 0,
    AUDIO_PULSE,
    AUDIO_BROWSER,
    AUDIO_FILE,
    AUDIO_BOTH
} AudioBackend;

// Selects the routing (call before audio_mic_open/audio_spk_open)
bool           audio_set_backend(AudioBackend b);
AudioBackend   audio_parse_backend(const char *name);
const char    *audio_backend_name(AudioBackend b);
AudioBackend   audio_get_backend(void);
void           audio_backend_status(char *out, int len);   // human readable

// Public API used by the M17 code (implemented by audio.cpp, the dispatcher)
void audio_mic_open(void);
void audio_mic_close(void);
void audio_spk_open(void);
void audio_spk_close(void);
void audio_input(int16_t *s, int len);          // blocks until len samples or timeout
void audio_output(int16_t *s, int len);
void audio_input_flush(void);
void audio_output_flush(void);
double audio_get_mic_level(void);               // 0..1 RMS for the UI meter
double audio_get_spk_level(void);

// ---- PulseAudio back end (audio_pulse.cpp, needs -lplus-simple) -----------
void audio_pulse_mic_open(void);
void audio_pulse_mic_close(void);
void audio_pulse_spk_open(void);
void audio_pulse_spk_close(void);
void audio_pulse_input(int16_t *s, int len);
void audio_pulse_output(int16_t *s, int len);
bool audio_pulse_ready(void);

// ---- Browser back end (audio_web.cpp) -------------------------------------
void audio_web_mic_open(void);
void audio_web_mic_close(void);
void audio_web_spk_open(void);
void audio_web_spk_close(void);
void audio_web_mic_push(const int16_t *s, int len);   // from the WebSocket client
int  audio_web_mic_available(void);
int  audio_web_input(int16_t *s, int len);            // returns samples delivered
void audio_web_output(int16_t *s, int len);           // forwarded to the browser
bool audio_web_active(void);

// WebSocket sink used by the browser back end, implemented in webui.cpp
void webui_audio_to_browser(const int16_t *s, int len);

// ---- File back end (audio_file.cpp) --------------------------------------
void audio_file_set(const char *in_path, const char *out_path);
void audio_file_mic_open(void);
void audio_file_mic_close(void);
void audio_file_spk_open(void);
void audio_file_spk_close(void);
void audio_file_input(int16_t *s, int len);
void audio_file_output(int16_t *s, int len);
bool audio_file_eof(void);
long audio_file_frames_read(void);
long audio_file_frames_written(void);

#endif // AUDIO_H
