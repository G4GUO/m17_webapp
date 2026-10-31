// m17_sms.cpp
//
// SMS over M17 - application data type 0x05, a null terminated UTF-8 message.
//
// Transmit is queued and sent from its own thread so a browser or console never
// waits on a burst, and so an SMS and an AX.25 frame cannot interleave on air
// (both go through the same burst lock in m17_ax25.cpp).  Receive keeps the last
// few messages for the web interface, which is the only place they are shown.
//
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "m17defines.h"
#include "m17_ax25.h"
#include "m17_sms.h"

extern "C" {
#include "m17_packet.h"      /* M17_MAX_APP_BYTES */
}

/* --------------------------------------------------------------- receive */

static SmsMsg          m_rx[M17_SMS_KEEP];
static int             m_rx_head;      /* index of the newest entry */
static int             m_rx_count;
static pthread_mutex_t m_rx_lock = PTHREAD_MUTEX_INITIALIZER;

/* --------------------------------------------------------------- transmit */

#define SMS_QUEUE 4

static char            m_q[SMS_QUEUE][M17_SMS_MAX_TEXT + 1];
static int             m_q_head;
static int             m_q_count;
static pthread_mutex_t m_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       m_thread;
static int             m_thread_started;
static volatile int    m_run;

static void stamp(char *out, size_t n)
{
    time_t    now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(out, n, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void m17_sms_rx_packet(const uint8_t *data, int len)
{
    SmsMsg m;
    const M17_Dbase *db;
    int    i;

    if (data == NULL || len < 2 || data[0] != M17_TYPE_SMS)
        return;

    memset(&m, 0, sizeof m);
    stamp(m.time, sizeof m.time);

    /* The message is null terminated inside the payload; a sender that forgot
     * the terminator still gets its bytes shown rather than nothing. */
    for (i = 1; i < len && m.len < M17_SMS_MAX_TEXT; i++) {
        uint8_t c = data[i];
        if (c == 0)
            break;
        /* Keep it printable: a stray control character would be written into the
         * JSON state and break the page. */
        if (c < 0x20 && c != '\n' && c != '\t')
            c = '.';
        m.text[m.len++] = (char)c;
    }
    m.text[m.len] = 0;
    if (m.len == 0)
        return;

    /* The LSF source is the station that keyed the burst, which for SMS is the
     * originator - unlike AX.25, where the frame header carries it. */
    db = m17_get_db();
    if (db != NULL) {
        snprintf(m.src, sizeof m.src, "%.9s", db->rx_src_add);
        for (char *q = m.src; *q; q++) *q = (char)toupper((unsigned char)*q);
        for (int k = (int)strlen(m.src) - 1; k >= 0 && m.src[k] == ' '; k--)
            m.src[k] = 0;
    }
    if (m.src[0] == 0) {
        /* A loopback burst has no link setup frame, so there is no source to
         * read - and the honest answer there is our own callsign, because we
         * are the station that "sent" it. */
        if (m17_ax25_loopback() && db != NULL) {
            char own[16] = "";
            m17_decode_call(m17_db_get_src(), own);
            snprintf(m.src, sizeof m.src, "%.9s", own);
            for (char *q = m.src; *q; q++) *q = (char)toupper((unsigned char)*q);
            for (int k = (int)strlen(m.src) - 1; k >= 0 && m.src[k] == ' '; k--)
                m.src[k] = 0;
        }
        if (m.src[0] == 0)
            snprintf(m.src, sizeof m.src, "UNKNOWN");
    }

    printf("m17web: RX  SMS %d bytes from %s\n", m.len, m.src);
    fflush(stdout);
    gui_log_event("rx", "RX  SMS %d bytes from %s", m.len, m.src);

    pthread_mutex_lock(&m_rx_lock);
    m_rx_head = (m_rx_head + 1) % M17_SMS_KEEP;
    m_rx[m_rx_head] = m;
    if (m_rx_count < M17_SMS_KEEP)
        m_rx_count++;
    pthread_mutex_unlock(&m_rx_lock);

    gui_sms_received(&m);
}

int m17_sms_count(void)
{
    int n;
    pthread_mutex_lock(&m_rx_lock);
    n = m_rx_count;
    pthread_mutex_unlock(&m_rx_lock);
    return n;
}

const SmsMsg *m17_sms_get(int i)
{
    static SmsMsg out;
    int idx;
    const SmsMsg *r = NULL;

    pthread_mutex_lock(&m_rx_lock);
    if (i >= 0 && i < m_rx_count) {
        idx  = (m_rx_head - i + M17_SMS_KEEP * 2) % M17_SMS_KEEP;
        out  = m_rx[idx];
        r    = &out;
    }
    pthread_mutex_unlock(&m_rx_lock);
    return r;
}

int m17_sms_pending(void)
{
    int n;
    pthread_mutex_lock(&m_q_lock);
    n = m_q_count;
    pthread_mutex_unlock(&m_q_lock);
    return n;
}

/* ------------------------------------------------------------------ send */

int m17_sms_queue(const char *text)
{
    size_t n;

    if (text == NULL)
        return -1;
    while (*text == ' ')                    /* a command line pad is not text */
        text++;
    n = strlen(text);
    if (n == 0)
        return -1;

    pthread_mutex_lock(&m_q_lock);
    if (m_q_count >= SMS_QUEUE) {
        pthread_mutex_unlock(&m_q_lock);
        return -2;
    }
    /* Truncate rather than refuse: an over-long message still goes out, and the
     * sender is told what happened by the console line below. */
    snprintf(m_q[(m_q_head + m_q_count) % SMS_QUEUE], M17_SMS_MAX_TEXT + 1, "%s", text);
    m_q_count++;
    pthread_mutex_unlock(&m_q_lock);
    return 0;
}

static int take(char *out)
{
    int got = 0;

    pthread_mutex_lock(&m_q_lock);
    if (m_q_count > 0) {
        memcpy(out, m_q[m_q_head], M17_SMS_MAX_TEXT + 1);
        m_q_head = (m_q_head + 1) % SMS_QUEUE;
        m_q_count--;
        got = 1;
    }
    pthread_mutex_unlock(&m_q_lock);
    return got;
}

static void *sms_thread(void *arg)
{
    static uint8_t blk[M17_MAX_APP_BYTES + 4];
    char   text[M17_SMS_MAX_TEXT + 1];
    int    n, st;

    (void)arg;
    while (m_run) {
        if (!take(text)) {
            usleep(100000);
            continue;
        }
        n = (int)strlen(text);
        if (n > M17_SMS_MAX_TEXT) {
            n = M17_SMS_MAX_TEXT;
            text[n] = 0;
        }

        /* Application block: type specifier, the message, and the terminator the
         * specification asks for.  The M17 CRC and the 25-byte chunking are the
         * frame builder's job, exactly as they are for AX.25. */
        blk[0] = M17_TYPE_SMS;
        memcpy(&blk[1], text, (size_t)n);
        blk[1 + n] = 0;

        printf("m17web: TX  SMS %d bytes  (onto air)\n", n);
        fflush(stdout);
        gui_log_event("tx", "TX  SMS %d bytes onto air", n);

        st = m17_send_app_burst(blk, n + 2);
        if (st == -2) {
            /* The transmitter was busy with voice: give it a moment and try
             * again rather than dropping the message. */
            pthread_mutex_lock(&m_q_lock);
            m_q_head = (m_q_head - 1 + SMS_QUEUE) % SMS_QUEUE;
            m_q_count++;
            pthread_mutex_unlock(&m_q_lock);
            usleep(500000);
        } else if (st != 0) {
            printf("m17web: SMS not transmitted (%d)\n", st);
            fflush(stdout);
            gui_log_event("err", "SMS not transmitted (%d)", st);
        }
    }
    return NULL;
}

int m17_sms_init(void)
{
    if (m_thread_started)
        return 0;
    m_run = 1;
    if (pthread_create(&m_thread, NULL, sms_thread, NULL) != 0) {
        m_run = 0;
        return -1;
    }
    m_thread_started = 1;
    return 0;
}
