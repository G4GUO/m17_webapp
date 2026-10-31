// m17_sms.h
//
// SMS over M17: application data type 0x05, a null terminated UTF-8 message
// (M17 specification 2.0.10, Table 3.15).
//
// The message rides in the same packet burst as AX.25 - preambles, the link
// setup frame, 25-byte packet frames, end of transmission - so everything that
// was learnt about getting a packet across applies unchanged: the type byte is
// part of the CRC input, and a frame that loses its tail loses its CRC.
//
#ifndef M17_SMS_H
#define M17_SMS_H

#include <stdint.h>
#include <stddef.h>

/* Application data type for a text message. */
#define M17_TYPE_SMS      0x05

/* 823 bytes of application data (section 2.9) carry the type specifier, the
 * message and nothing else - the M17 CRC is appended by the frame builder after
 * it.  So 821 characters fit.  A longer message is truncated rather than
 * rejected: a short message that arrives beats a long one that does not. */
#define M17_SMS_MAX_TEXT  821

/* How many received messages are kept for the web interface. */
#define M17_SMS_KEEP      32

typedef struct {
    char time[16];                    /* HH:MM:SS local, when it was heard */
    char src[16];                     /* the station that keyed the burst */
    char text[M17_SMS_MAX_TEXT + 1];
    int  len;
} SmsMsg;

/* Nothing to start: transmission is serviced from the transmitter thread by
 * m17_sms_service(), the same place AX.25 is serviced from. */
int  m17_sms_init(void);

/* Transmit one queued message.  1 if a burst was sent, 0 otherwise. */
int  m17_sms_service(void);

/* Queue a message for transmission.  0 queued, -1 empty, -2 queue full. */
int  m17_sms_queue(const char *text);

/* Called from the receive path with the whole application block: data[0] is
 * the type specifier, which the caller has already checked is 0x05. */
void m17_sms_rx_packet(const uint8_t *data, int len);

int  m17_sms_count(void);
const SmsMsg *m17_sms_get(int i);     /* 0 = newest; NULL past the end */
int  m17_sms_pending(void);

/* Provided by m17_ax25.cpp: put an application block on air as a packet burst.
 * 0 sent, -1 bad block, -2 the transmitter is busy with voice. */
int  m17_send_app_burst(const uint8_t *app, int n);

/* Provided by webui.cpp: tell the browser about a message. */
void gui_sms_received(const SmsMsg *m);
void gui_sms_replay(int id);          /* send the kept messages to one client */

#endif
