//
// feather_test.cpp
//
// The Feather back end, driven over a real serial port against a fake board, so
// the wiring can be checked without the hardware.
//
// The fake board is deliberately the supervisor's rules and nothing else: it
// echoes dibits back as symbols, only while the host holds PTT and TXEN, and it
// sends status.  What this proves is the seam - that the program's transmit path
// reaches the wire in the format the firmware parses, and that symbols coming
// back reach m17_rx_symbols() in order and at the right amplitude.  The framing
// on the far side of m17_rx_symbols is exercised by the host tests in
// m17_feather, and the board's side of the wire by its own tests.
//
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "feather.h"
#include "feather_link.h"

// ------------------------------------------------------------------ stubs
// feather.cpp only needs these two; the real ones live in m17_rx_frame.cpp,
// which would drag the whole program in with it.
static float   g_sym[4096];
static int     g_sym_n;
static bool    g_lock;
static pthread_mutex_t g_sym_lock = PTHREAD_MUTEX_INITIALIZER;

// C++ linkage, matching the declaration in m17defines.h that feather.cpp sees.
void m17_rx_symbols(float *sym, int len)
{
    pthread_mutex_lock(&g_sym_lock);
    for (int i = 0; i < len && g_sym_n < (int)(sizeof(g_sym)/sizeof(g_sym[0])); i++)
        g_sym[g_sym_n++] = sym[i];
    pthread_mutex_unlock(&g_sym_lock);
}

bool m17_rx_lock(void) { return g_lock; }

// ui.h's log hook.  The real one goes to the console and the web UI; here it is
// just another line of test output, which is where a "board connected" line is
// actually useful.
void gui_log_event(const char *kind, const char *fmt, ...)
{
    va_list ap;
    (void)kind;
    printf("  log: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

// ------------------------------------------------------------------- helpers
static int      g_master = -1;
static volatile bool g_board_run;
static volatile bool g_board_keyed;
static volatile uint32_t g_board_frames;
static uint8_t  g_board_dibits[4096];
static int      g_board_n;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static int check_count, fail_count;

// Where the echoed block starts: the first symbol with any amplitude at or after
// `from`.  The board sends silence until it has dibits, so the echo does not
// necessarily begin at the next symbol.
static int data_start(int from)
{
    int found = -1;
    pthread_mutex_lock(&g_sym_lock);
    for (int i = from; i < g_sym_n; i++)
        if (g_sym[i] > 0.01f || g_sym[i] < -0.01f) { found = i; break; }
    pthread_mutex_unlock(&g_sym_lock);
    return found;
}

// Symbols carrying data, i.e. not the silence the fake board sends when it has
// nothing queued.
static int nonzero_from(int from)
{
    int n = 0;
    pthread_mutex_lock(&g_sym_lock);
    for (int i = from; i < g_sym_n; i++)
        if (g_sym[i] > 0.01f || g_sym[i] < -0.01f) n++;
    pthread_mutex_unlock(&g_sym_lock);
    return n;
}

#define CHECK(cond, ...)                                     \
    do {                                                     \
        check_count++;                                       \
        if (!(cond)) {                                       \
            fail_count++;                                    \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
        }                                                    \
    } while (0)

// -------------------------------------------------------------------- board
static void board_send(uint8_t type, const uint8_t *payload, size_t len)
{
    static uint8_t frame[LINK_MAX_FRAME];
    static uint8_t seq;
    size_t n = link_encode(frame, sizeof(frame), type, seq++, payload, len);
    ssize_t w;

    if (n == 0) { printf("  board: encode failed\n"); return; }
    w = write(g_master, frame, n);
    if (w != (ssize_t)n && !(w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
        printf("  board: write failed: %s\n", strerror(errno));
}

static void board_dibits(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (g_board_n < (int)sizeof(g_board_dibits))
            g_board_dibits[g_board_n++] = d[i] & 3u;
    }
}

static void board_on_frame(void *ctx, uint8_t type, uint8_t seq,
                           const uint8_t *p, size_t len)
{
    (void)ctx; (void)seq;

    switch (type) {
    case LINK_TX_CONTROL: {
        uint8_t f = len ? p[0] : 0;
        g_board_keyed = (f & LINK_CTL_PTT) && (f & LINK_CTL_TXEN);
        g_lock = (f & LINK_CTL_RX_LOCK) != 0;
        break;
    }
    case LINK_TX_DIBITS: {
        uint8_t d[LINK_SYMBOLS_PER_FRAME];
        size_t n = link_unpack_dibits(d, p, LINK_SYMBOLS_PER_FRAME);
        g_board_frames++;
        board_dibits(d, n);
        break;
    }
    case LINK_PING: {
        uint8_t echo[4] = { 1, 2, 3, 4 };
        board_send(LINK_PONG, echo, sizeof(echo));
        break;
    }
    default:
        break;
    }
}

static void *board_thread(void *arg)
{
    static const float amp[4] = { 0.333f, 1.0f, -0.333f, -1.0f };
    static uint8_t rxbuf[4096];
    static uint8_t status[12];
    link_parser_t parser;
    uint64_t next_block = now_ms() + 40;
    uint64_t next_status = now_ms() + 250;

    (void)arg;
    link_parser_reset(&parser);

    while (g_board_run) {
        ssize_t n = read(g_master, rxbuf, sizeof(rxbuf));
        if (getenv("FT_DEBUG"))
            printf("  dbg t=%llu read=%ld errno=%d frames=%u keyed=%d\n",
                   (unsigned long long)(now_ms() % 100000), (long)n, errno,
                   g_board_frames, (int)g_board_keyed);
        if (n > 0) link_parser_feed(&parser, rxbuf, (size_t)n, board_on_frame, NULL);

        uint64_t t = now_ms();

        if (t >= next_block) {
            // One block of 192 symbols every 40 ms, always: the board's receiver
            // runs whether or not the transmitter does, so symbols flow for as
            // long as a host is there.  PTT gates the DAC and the radio, not the
            // ADC.  With nothing queued the block is silence - a real board
            // sends discriminator noise there, which for this test would only
            // make the expected values uninteresting.
            float sym[LINK_SYMBOLS_PER_FRAME];
            uint8_t payload[LINK_SYMBOLS_PER_FRAME + 1];
            bool echo = g_board_n >= (int)LINK_SYMBOLS_PER_FRAME;

            pthread_mutex_lock(&g_sym_lock);
            for (int i = 0; i < (int)LINK_SYMBOLS_PER_FRAME; i++)
                sym[i] = echo ? amp[g_board_dibits[i]] : 0.0f;
            pthread_mutex_unlock(&g_sym_lock);

            if (echo) {
                memmove(g_board_dibits, g_board_dibits + LINK_SYMBOLS_PER_FRAME,
                        (size_t)(g_board_n - (int)LINK_SYMBOLS_PER_FRAME));
                g_board_n -= (int)LINK_SYMBOLS_PER_FRAME;
            }

            size_t plen = link_pack_symbols(payload, sym, LINK_SYMBOLS_PER_FRAME);
            board_send(LINK_RX_SYMBOLS, payload, plen);
            next_block += 40;
        }

        if (t >= next_status) {
            memset(status, 0, sizeof(status));
            status[0] = g_board_keyed ? 1 : 0;   // PTT
            status[1] = 1;                       // host present
            status[2] = g_lock ? 1 : 0;
            board_send(LINK_RX_STATUS, status, sizeof(status));
            next_status += 250;
        }

        struct timespec ts = { 0, 2000000 };     // 2 ms
        nanosleep(&ts, NULL);
    }
    return NULL;
}

// --------------------------------------------------------------------- main
int main(void)
{
    char slave[128];
    // Non-blocking: the board writes whether or not the host is polling yet, and
    // a blocking write into a full pty would wedge the thread before it ever got
    // to read a frame.
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    const uint8_t frame_dibits[LINK_SYMBOLS_PER_FRAME] = {
        // A recognisable pattern rather than all one value: the sync word
        // 0x77 0x77 0x55 0x5D-ish alternation would hide an off-by-one.
        1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 3, 2, 1, 0,
    };
    uint8_t dibits[LINK_SYMBOLS_PER_FRAME];
    pthread_t th;

    printf("feather back end over a pty\n");

    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 ||
        ptsname_r(master, slave, sizeof(slave)) != 0) {
        printf("  FATAL: cannot make a pty: %s\n", strerror(errno));
        return 1;
    }

    struct termios tio;
    if (tcgetattr(master, &tio) == 0) {
        cfmakeraw(&tio);
        tcsetattr(master, TCSANOW, &tio);
    }

    g_master = master;
    CHECK(feather_open(slave), "feather_open(%s)", slave);
    CHECK(feather_open_ok(), "port open");

    g_board_run = true;
    pthread_create(&th, NULL, board_thread, NULL);

    // ------------------------------------------------------------------ idle
    for (int i = 0; i < 60; i++) { feather_poll(); usleep(2000); }
    CHECK(!g_board_keyed, "board keyed while the host had not asked");
    CHECK(g_sym_n > 0, "no symbol stream at all while idle");
    CHECK(nonzero_from(0) == 0, "%d nonzero symbols with nothing transmitted",
          nonzero_from(0));

    // ------------------------------------------------------ transmit a frame
    for (int i = 0; i < (int)LINK_SYMBOLS_PER_FRAME; i++)
        dibits[i] = frame_dibits[i % 16];

    int base = g_sym_n;                 // the idle stream is already flowing
    feather_key(true);
    feather_tx_dibits(dibits, LINK_SYMBOLS_PER_FRAME);

    // Wait for the echo, not for "some symbols": the stream never stops.
    uint64_t t0 = now_ms();
    while (now_ms() - t0 < 1000 &&
           nonzero_from(base) < (int)LINK_SYMBOLS_PER_FRAME) {
        feather_poll();
        usleep(1000);
    }

    CHECK(g_board_frames == 1, "board got %u dibit frames, wanted 1", g_board_frames);
    CHECK(g_board_keyed, "board was not keyed after PTT+TXEN");
    CHECK(g_sym_n >= (int)LINK_SYMBOLS_PER_FRAME,
          "only %d of %d symbols came back", g_sym_n, (int)LINK_SYMBOLS_PER_FRAME);

    // The echo is the same dibits, mapped to amplitudes: check order and value,
    // which is what an off-by-one in the packing or the symbol scale would break.
    {
        int start = data_start(base);
        int bad = 0;

        CHECK(start >= 0, "no echoed symbols after the frame was sent");
        for (int i = 0; start >= 0 && i < (int)LINK_SYMBOLS_PER_FRAME &&
                            start + i < g_sym_n; i++) {
            float want = (dibits[i] == 0) ? 0.333f
                       : (dibits[i] == 1) ? 1.0f
                       : (dibits[i] == 2) ? -0.333f : -1.0f;
            if (g_sym[start + i] < want - 0.02f || g_sym[start + i] > want + 0.02f) {
                if (bad < 4)
                    printf("  sym[%d] = %+.3f, wanted %+.3f (dibit %d)\n",
                           i, g_sym[start + i], want, dibits[i]);
                bad++;
            }
        }
        CHECK(bad == 0, "%d of %d symbols wrong", bad, (int)LINK_SYMBOLS_PER_FRAME);
    }

    CHECK(feather_symbols_rx() >= LINK_SYMBOLS_PER_FRAME,
          "counted %u symbols", feather_symbols_rx());
    CHECK(feather_frames_tx() == 1, "counted %u transmitted frames", feather_frames_tx());
    CHECK(feather_drops() == 0, "%u drops", feather_drops());

    // ------------------------------------------------------------- unkeying
    feather_key(false);
    for (int i = 0; i < 40; i++) { feather_poll(); usleep(2000); }   // let it drain
    int before = g_sym_n;
    for (int i = 0; i < 60; i++) { feather_poll(); usleep(2000); }
    CHECK(!g_board_keyed, "board still keyed after unkey");
    CHECK(nonzero_from(before) == 0,
          "%d symbols of data arrived after unkey", nonzero_from(before));

    // --------------------------------------------------------------- status
    for (int i = 0; i < 60; i++) { feather_poll(); usleep(2000); }
    printf("  %s\n", feather_status());
    CHECK(feather_silent_ms() < 500, "link looked silent: %u ms", feather_silent_ms());

    feather_close();
    g_board_run = false;
    pthread_join(th, NULL);
    close(master);

    printf("%d checks, %d failed\n", check_count, fail_count);
    return fail_count ? 1 : 0;
}
