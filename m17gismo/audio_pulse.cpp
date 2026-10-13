// audio_pulse.cpp
//
// PulseAudio back end (was audio_io.cpp upstream).  Only compiled when
// HAVE_PULSE is defined (i.e. libpulse-simple is available).
//
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

#include <pulse/simple.h>
#include <pulse/error.h>

#include "m17defines.h"
#include "audio.h"

#define RATE 8000

static pa_simple *m_os = NULL;
static pa_simple *m_is = NULL;

static const pa_sample_spec spec = {
	.format		= PA_SAMPLE_S16LE,
	.rate		= RATE,
	.channels	= 1,
};

bool audio_pulse_ready(void) { return (m_os != NULL) || (m_is != NULL); }

void audio_pulse_mic_open(void){
	m_is = NULL;
	m_is = pa_simple_new(NULL, "m17web", PA_STREAM_RECORD, NULL, "m17web", &spec, NULL, NULL, NULL);
	if (!m_is) printf("Audio Input failed\n");
}

void audio_pulse_spk_open(void){
	m_os = NULL;
	m_os = pa_simple_new(NULL, "m17web", PA_STREAM_PLAYBACK, NULL, "m17web", &spec, NULL, NULL, NULL);
	if (!m_os) printf("Audio Output failed\n");
}

void audio_pulse_mic_close(void){
	if(m_is != NULL){
		pa_simple_free(m_is);
		m_is = NULL;
	}
}

void audio_pulse_spk_close(void){
	if(m_os != NULL ){
		pa_simple_free(m_os);
		m_os = NULL;
	}
}

void audio_pulse_output(int16_t *s, int len){
	if(m_os != NULL){
		pa_simple_write(m_os, s, len*sizeof(int16_t), NULL);
	}
}

void audio_pulse_input(int16_t *s, int len){
	if(m_is != NULL){
		pa_simple_read(m_is, s, len*sizeof(int16_t), NULL);
	}
}

void audio_pulse_flush(void){
	int e;
	if(m_is != NULL){
		pa_simple_flush(m_is, &e);
	}
	if(m_os != NULL){
		pa_simple_flush(m_os, &e);
	}
}
