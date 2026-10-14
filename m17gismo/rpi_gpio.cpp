// rpi_gpio.cpp
//
// Hardware PTT on the Raspberry Pi's 40 pin header:
//
//   PTT_OUT (BCM 10)  output, driven high while transmitting and low otherwise,
//                     to key an external device
//   PTT_IN  (BCM 11)  input, a switch to ground: pressed reads low, so the line
//                     is requested with the internal pull-up and as active low,
//                     which makes "active" mean "pressed"
//
// This used to use the legacy /sys/class/gpio interface.  That interface is
// closed on current kernels - writing to /sys/class/gpio/export returns EINVAL
// even as root - so it now uses libgpiod and the character device
// /dev/gpiochip0.  Built without libgpiod it becomes a stub that says so,
// because the station has to keep working without hardware PTT: the button in
// the web interface remains the way to transmit.
//
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "m17defines.h"

#define PTT_OUT      10
#define PTT_IN       11
#define GPIO_CHIP    "/dev/gpiochip0"
#define GPIO_CONSUMER "m17web"

static char m_error[192] = "hardware PTT has not been opened";
static bool m_out_ok;
static bool m_in_ok;
static bool m_pressed;

#ifdef HAVE_GPIOD
#include <gpiod.h>

static struct gpiod_chip         *m_chip;
static struct gpiod_line_request *m_out_req;
static struct gpiod_line_request *m_in_req;

static struct gpiod_line_request *gpio_claim(int offset, bool output) {
    struct gpiod_line_settings  *set  = gpiod_line_settings_new();
    struct gpiod_line_config    *lcfg = gpiod_line_config_new();
    struct gpiod_request_config *rcfg = gpiod_request_config_new();
    struct gpiod_line_request   *req  = NULL;
    unsigned int off = (unsigned int)offset;

    if (!set || !lcfg || !rcfg) goto done;

    if (output) {
        gpiod_line_settings_set_direction(set, GPIOD_LINE_DIRECTION_OUTPUT);
        gpiod_line_settings_set_output_value(set, GPIOD_LINE_VALUE_INACTIVE);
    } else {
        gpiod_line_settings_set_direction(set, GPIOD_LINE_DIRECTION_INPUT);
        gpiod_line_settings_set_bias(set, GPIOD_LINE_BIAS_PULL_UP);
        gpiod_line_settings_set_active_low(set, true);   // a switch to ground
    }

    if (gpiod_line_config_add_line_settings(lcfg, &off, 1, set) < 0) goto done;
    gpiod_request_config_set_consumer(rcfg, GPIO_CONSUMER);
    req = gpiod_chip_request_lines(m_chip, rcfg, lcfg);

done:
    if (set)  gpiod_line_settings_free(set);
    if (lcfg) gpiod_line_config_free(lcfg);
    if (rcfg) gpiod_request_config_free(rcfg);
    return req;
}
#endif

int rpi_gpio_open(void) {
#ifdef HAVE_GPIOD
    if (m_out_ok && m_in_ok) return 0;                 // already claimed

    if (!m_chip) {
        m_chip = gpiod_chip_open(GPIO_CHIP);
        if (!m_chip) {
            snprintf(m_error, sizeof(m_error), "%s: %s", GPIO_CHIP, strerror(errno));
            printf("m17web: hardware PTT unavailable (%s)\n", m_error);
            return -1;
        }
    }
    if (!m_out_req) m_out_req = gpio_claim(PTT_OUT, true);
    if (!m_in_req)  m_in_req  = gpio_claim(PTT_IN, false);
    m_out_ok = (m_out_req != NULL);
    m_in_ok  = (m_in_req  != NULL);

    if (m_out_ok && m_in_ok) {
        m_error[0] = 0;
        printf("m17web: hardware PTT on %s: GPIO %d output, GPIO %d input (pressed reads low)\n",
               GPIO_CHIP, PTT_OUT, PTT_IN);
        return 0;
    }
    snprintf(m_error, sizeof(m_error), "GPIO %d %s, GPIO %d %s",
             PTT_OUT, m_out_ok ? "claimed" : "not available",
             PTT_IN,  m_in_ok  ? "claimed" : "not available");
    printf("m17web: hardware PTT incomplete (%s)\n", m_error);
    return m_in_ok ? -2 : -1;
#else
    snprintf(m_error, sizeof(m_error),
             "built without libgpiod (install libgpiod-dev and rebuild)");
    return -1;
#endif
}

void rpi_tx(void) {
#ifdef HAVE_GPIOD
    if (m_out_req &&
        gpiod_line_request_set_value(m_out_req, PTT_OUT, GPIOD_LINE_VALUE_ACTIVE) < 0) {
        snprintf(m_error, sizeof(m_error), "GPIO %d output: %s", PTT_OUT, strerror(errno));
    }
#endif
}

void rpi_rx(void) {
#ifdef HAVE_GPIOD
    if (m_out_req &&
        gpiod_line_request_set_value(m_out_req, PTT_OUT, GPIOD_LINE_VALUE_INACTIVE) < 0) {
        snprintf(m_error, sizeof(m_error), "GPIO %d output: %s", PTT_OUT, strerror(errno));
    }
#endif
}

bool rpi_read_ptt(void) {
#ifdef HAVE_GPIOD
    if (!m_in_req) return false;
    int v = gpiod_line_request_get_value(m_in_req, PTT_IN);
    if (v < 0) {
        snprintf(m_error, sizeof(m_error), "GPIO %d input: %s", PTT_IN, strerror(errno));
        m_in_ok = false;
        return false;
    }
    m_pressed = (v == GPIOD_LINE_VALUE_ACTIVE);
    return m_pressed;
#else
    return false;
#endif
}

bool rpi_gpio_ok(void) {
    return m_out_ok && m_in_ok;
}

bool rpi_gpio_pressed(void) {
    return m_pressed;
}

int rpi_gpio_out_pin(void) { return PTT_OUT; }
int rpi_gpio_in_pin(void)  { return PTT_IN; }

const char *rpi_gpio_error(void) {
    return m_error;
}

void rpi_gpio_close(void) {
#ifdef HAVE_GPIOD
    if (m_out_req) { gpiod_line_request_release(m_out_req); m_out_req = NULL; }
    if (m_in_req)  { gpiod_line_request_release(m_in_req);  m_in_req  = NULL; }
    if (m_chip)    { gpiod_chip_close(m_chip);              m_chip    = NULL; }
#endif
    m_out_ok = m_in_ok = false;
    m_pressed = false;
}
