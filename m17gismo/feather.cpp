//
// feather.c
//
// The Feather STM32F405 modem as a radio back end.  See feather.h for the shape.
//
// This file is deliberately small.  Everything that could be decided without a
// serial port - the wire format, the policy about when to key, the symbol
// packing - was decided and tested on the other side of the link, in the
// m17_feather firmware tree (link/link.c, fw/supervisor.c, tests/).  What is left
// here is transport, a heartbeat, and the two calls into the decoder.
//
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "m17defines.h"
#include "ui.h"
#include "feather.h"
#include "feather_link.h"

#define FEATHER_CTL_MS     40u     // board times the host out at 100 ms
#define FEATHER_READ_MAX   4096

static int            m_fd = -1;
static bool           m_keyed;
static bool           m_ever_open;
static bool           m_link_up;
static uint8_t        m_seq;
static uint64_t       m_last_ctl;
static uint64_t       m_last_rx;
static link_parser_t  m_parser;

static pthread_mutex_t m_tx_lock = PTHREAD_MUTEX_INITIALIZER;

static float   m_sym[LINK_SYMBOLS_PER_FRAME];
static uint8_t m_rxbuf[FEATHER_READ_MAX];
static uint8_t m_frame[LINK_MAX_FRAME];

static struct {
    uint32_t symbols;
    uint32_t frames_in;
    uint32_t frames_out;
    uint32_t drops;            // write failures, short writes, bad frames
    uint32_t overruns;         // board reports its FIFO overflowed
    uint32_t underruns;        // board reports it ran dry mid-frame
    uint8_t  st[12];           // last LINK_RX_STATUS payload
} m_st;

static char m_status[192] = "feather: not in use";

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

// ---------------------------------------------------------------------- write
// Caller holds m_tx_lock.
static bool wr_all(const uint8_t *p, size_t n)
{
    size_t off = 0;

    while (off < n) {
        ssize_t w = write(m_fd, p + off, n - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Should not happen at 2.5 kB/s, but do not spin forever either.
            struct timespec ts = { 0, 500000 };   // 0.5 ms
            nanosleep(&ts, NULL);
            if (++off > 4 * n) break;             // give up: count as a drop
            continue;
        }
        return false;
    }
    return off == n;
}

static bool send_frame(uint8_t type, const uint8_t *payload, size_t len)
{
    size_t n;
    bool ok;

    pthread_mutex_lock(&m_tx_lock);
    n = link_encode(m_frame, sizeof(m_frame), type, m_seq++, payload, len);
    ok = (n > 0) && wr_all(m_frame, n);
    pthread_mutex_unlock(&m_tx_lock);
    if (!ok) m_st.drops++;
    return ok;
}

// ------------------------------------------------------------------- control
// PTT and TXEN together, or the board will not key.  RX_LOCK tells the board the
// host framer is in sync, so it can slow its timing loop down.
static void send_control(void)
{
    uint8_t flags = 0;

    if (m_keyed) flags |= LINK_CTL_PTT | LINK_CTL_TXEN;
    if (m17_rx_lock()) flags |= LINK_CTL_RX_LOCK;

    m_last_ctl = now_ms();
    (void)send_frame(LINK_TX_CONTROL, &flags, 1);
}

// ------------------------------------------------------------------ receive
static void on_frame(void *ctx, uint8_t type, uint8_t seq,
                     const uint8_t *payload, size_t len)
{
    (void)ctx;
    (void)seq;

    switch (type) {
    case LINK_RX_SYMBOLS: {
        size_t n = link_unpack_symbols(m_sym, payload, len);

        if (n == 0 || n > LINK_SYMBOLS_PER_FRAME) {
            // A malformed block is a link fault, not a decoding problem: throw it
            // away rather than hand the framer a short frame, which would look
            // like a symbol slip.
            m_st.drops++;
            break;
        }
        m_st.frames_in++;
        m_st.symbols += (uint32_t)n;
        m_last_rx = now_ms();
        m17_rx_symbols(m_sym, (int)n);
        break;
    }
    case LINK_RX_STATUS: {
        size_t n = len < sizeof(m_st.st) ? len : sizeof(m_st.st);
        memcpy(m_st.st, payload, n);
        if (len >= 6) {
            m_st.underruns = payload[4];
            m_st.overruns  = payload[5];
        }
        m_last_rx = now_ms();
        break;
    }
    case LINK_PONG:
        m_last_rx = now_ms();
        break;
    default:
        break;
    }
}

void feather_poll(void)
{
    if (m_fd < 0) return;

    for (;;) {
        ssize_t n = read(m_fd, m_rxbuf, sizeof(m_rxbuf));
        if (n > 0) {
            link_parser_feed(&m_parser, m_rxbuf, (size_t)n, on_frame, NULL);
            continue;
        }
        if (n == 0) break;                        // nothing waiting
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        // EIO on a CDC device usually means the board went away.
        close(m_fd);
        m_fd = -1;
        m_keyed = false;
        snprintf(m_status, sizeof(m_status), "feather: %s lost", FEATHER_DEFAULT_TTY);
        return;
    }

    // Say so when the board arrives or disappears, rather than leaving the
    // operator to wonder why there is no audio.
    {
        bool up = (now_ms() - m_last_rx) < 500;

        if (up != m_link_up) {
            m_link_up = up;
            gui_log_event("feather", "feather: board %s", up ? "connected" : "silent");
        }
    }

    if (now_ms() - m_last_ctl >= FEATHER_CTL_MS) send_control();
}

// ----------------------------------------------------------------- transmit
void feather_tx_dibits(const uint8_t *dibits, int n)
{
    uint8_t packed[LINK_DIBIT_BYTES];

    if (n <= 0) return;
    if (m_fd < 0) { m_st.drops++; return; }

    // One frame at a time: the board's FIFO is two frames deep, and it emits one
    // symbol per 208 us, so a 192-dibit frame is exactly 40 ms of air time.
    while (n > 0) {
        int take = n > (int)LINK_SYMBOLS_PER_FRAME ? (int)LINK_SYMBOLS_PER_FRAME : n;

        link_pack_dibits(packed, dibits, (size_t)take);
        if (send_frame(LINK_TX_DIBITS, packed, (size_t)((take + 3) / 4)))
            m_st.frames_out++;
        dibits += take;
        n -= take;
    }

    if (now_ms() - m_last_ctl >= FEATHER_CTL_MS) send_control();
}

// ---------------------------------------------------------------------- PTT
void feather_key(bool on)
{
    if (m_keyed == on) return;
    m_keyed = on;
    if (m_fd >= 0) send_control();
}

bool feather_keyed(void)
{
    return m_keyed;
}

// --------------------------------------------------------------------- open
bool feather_open(const char *path)
{
    struct termios tio;

    if (path == NULL) path = FEATHER_DEFAULT_TTY;

    if (m_fd >= 0) feather_close();

    m_ever_open = true;             // an attempt has been made: report its result
    m_fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (m_fd < 0) {
        snprintf(m_status, sizeof(m_status), "feather: cannot open %s: %s",
                 path, strerror(errno));
        return false;
    }

    // Raw: a CDC-ACM device still has a line discipline, and its default
    // settings would translate CR/LF and echo bytes back at us.  Binary link
    // frames do not survive that.
    if (tcgetattr(m_fd, &tio) == 0) {
        cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        (void)cfsetispeed(&tio, B115200);
        (void)cfsetospeed(&tio, B115200);
        (void)tcsetattr(m_fd, TCSANOW, &tio);
    }

    link_parser_reset(&m_parser);
    memset(&m_st, 0, sizeof(m_st));
    m_keyed = false;
    m_ever_open = true;
    m_last_ctl = 0;                 // send control on the first poll
    m_last_rx = now_ms();

    snprintf(m_status, sizeof(m_status), "feather: %s open, waiting for the board",
             path);
    return true;
}

void feather_close(void)
{
    if (m_fd >= 0) {
        m_keyed = false;
        (void)send_control();       // unkey on the way out
        close(m_fd);
    }
    m_fd = -1;
    m_keyed = false;
    snprintf(m_status, sizeof(m_status), "feather: closed");
}

bool feather_open_ok(void)
{
    return m_fd >= 0;
}

// ------------------------------------------------------------------- status
const char *feather_status(void)
{
    // "not in use" while the back end has never been selected, and the reason
    // the port would not open after it has - which is the message the operator
    // needs, so it must not be overwritten here.
    if (m_fd < 0) return m_status;

    snprintf(m_status, sizeof(m_status),
             "feather: %s  sym %u  tx %u  dr %u  u/r %u/%u  %s%s",
             m_st.st[1] ? "board ok" : "no board",
             (unsigned)m_st.symbols,
             (unsigned)m_st.frames_out,
             (unsigned)m_st.drops,
             (unsigned)m_st.underruns,
             (unsigned)m_st.overruns,
             m_st.st[0] ? "PTT " : "",
             m_st.st[2] ? "in-sync" : "");
    return m_status;
}

uint32_t feather_silent_ms(void)
{
    if (m_fd < 0) return 0xffffffffu;
    return (uint32_t)(now_ms() - m_last_rx);
}

uint32_t feather_symbols_rx(void) { return m_st.symbols; }
uint32_t feather_frames_tx(void)  { return m_st.frames_out; }
uint32_t feather_drops(void)      { return m_st.drops; }
