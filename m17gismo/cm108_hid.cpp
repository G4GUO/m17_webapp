//
// cm108_hid.cpp
//
// PTT on a CM108/CM119: the codec's HID interface, a five-byte vendor report,
// and GPIO3.  No kernel driver, because usbhid already binds the HID side and
// hands out /dev/hidrawN.
//
// The report, per the CM108/CM119 register layout as used by Dire Wolf, AllStar
// and every URI-alike since:
//
//   io[0] = 0
//   io[1] = 0
//   io[2] = data   bit(n-1) = level of GPIO n
//   io[3] = mask   bit(n-1) = 1 means GPIO n is an output
//   io[4] = 0
//
// Four bytes returns EPIPE; five is the length the chip wants.  The mask bit
// stays set on unkey - the pin remains an output, it just goes low - because
// handing it back to the chip as an input is how a fob ends up driving its own
// LED again.
//
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cm108_io.h"

struct cm108_hid {
    int  fd;
    char name[64];
};

static unsigned g_writes;

// Vendors whose codecs answer this report.  C-Media covers CM108/AH/B, CM119
// and A/B; the others are drop-in clones and the AIOC, which emulates a CM108
// on an STM32 and is the reason this list will keep growing.
static const char *const kVendors[] = { "00000D8C", "00000C76", "00001209" };

static const char *hid_name_from_uevent(const char *uevent, char *buf, size_t len)
{
    FILE *f = fopen(uevent, "r");
    if (f == NULL) return NULL;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "HID_ID=", 7) != 0) continue;
        // HID_ID=0003:00000D8C:0000013A   bus:vendor:product
        char bus[8] = {0}, vid[16] = {0};
        if (sscanf(line + 7, "%7[^:]:%15[^:]", bus, vid) != 2) break;
        if (strcmp(bus, "0003") != 0) break;              // USB only
        for (size_t i = 0; i < sizeof(kVendors) / sizeof(kVendors[0]); i++) {
            if (strcasecmp(vid, kVendors[i]) == 0) {
                snprintf(buf, len, "%s", vid);
                fclose(f);
                return buf;
            }
        }
        break;
    }
    fclose(f);
    return NULL;
}

void *cm108_hid_open(const char *path, char *err, int errlen)
{
    char found[64] = {0};
    char vid[32]   = {0};
    char node[64]  = {0};

    if (path == NULL || *path == '\0') {
        path = getenv("CM108_HID");
        if (path && *path) {
            snprintf(found, sizeof(found), "%s", path);
        } else {
            glob_t g;
            if (glob("/sys/class/hidraw/hidraw*", 0, NULL, &g) == 0) {
                for (size_t i = 0; i < g.gl_pathc; i++) {
                    char uevent[256];
                    snprintf(uevent, sizeof(uevent), "%s/device/uevent", g.gl_pathv[i]);
                    if (hid_name_from_uevent(uevent, vid, sizeof(vid)) == NULL) continue;
                    const char *slash = strrchr(g.gl_pathv[i], '/');
                    snprintf(node, sizeof(node), "/dev/%s", slash ? slash + 1 : "hidraw0");
                    snprintf(found, sizeof(found), "%s", node);
                    break;
                }
                globfree(&g);
            }
        }
    } else {
        snprintf(found, sizeof(found), "%s", path);
    }

    if (found[0] == '\0') {
        if (err && errlen) snprintf(err, (size_t)errlen, "no CM108/CM119 hidraw node found");
        return NULL;
    }

    // Non-blocking: a HID write can block on a device that is being unplugged,
    // and a stuck PTT key-up is worse than a missed one.
    int fd = open(found, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        if (err && errlen) snprintf(err, (size_t)errlen, "%s: %s", found, strerror(errno));
        return NULL;
    }

    cm108_hid *h = (cm108_hid *)calloc(1, sizeof(*h));
    if (h == NULL) { close(fd); return NULL; }
    h->fd = fd;
    snprintf(h->name, sizeof(h->name), "%s gpio", found);

    // Make sure the pin starts low: a fob that was unplugged mid-transmission
    // would otherwise come back keyed.
    cm108_hid_gpio(h, 3, 0);
    return h;
}

void cm108_hid_close(void *pv)
{
    cm108_hid *h = (cm108_hid *)pv;
    if (h == NULL) return;
    cm108_hid_gpio(h, 3, 0);
    close(h->fd);
    free(h);
}

int cm108_hid_gpio(void *pv, int pin, int on)
{
    cm108_hid *h = (cm108_hid *)pv;
    if (h == NULL || pin < 1 || pin > 8) return -1;

    unsigned char io[5];
    io[0] = 0;
    io[1] = 0;
    io[2] = (unsigned char)(on ? (1 << (pin - 1)) : 0);
    io[3] = (unsigned char)(1 << (pin - 1));      // 1 = output
    io[4] = 0;

    for (int guard = 0; guard < 4; guard++) {
        ssize_t n = write(h->fd, io, sizeof(io));
        if (n == (ssize_t)sizeof(io)) { g_writes++; return 0; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n < 0 && errno == EPIPE) return -1;    // wrong length: 5, not 4
        return -1;
    }
    return -1;
}

const char *cm108_hid_name(void *pv)
{
    cm108_hid *h = (cm108_hid *)pv;
    return h ? h->name : "none";
}

unsigned cm108_hid_writes(void) { return g_writes; }
