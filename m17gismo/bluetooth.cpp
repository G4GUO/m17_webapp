// bluetooth.cpp
//
// Bluetooth audio: choosing a speaker or headset and putting it into the audio
// path.  See bluetooth.h for what this is and why it is optional.
//
// The work is all in the ALSA device strings - BlueALSA has already turned the
// Bluetooth device into a PCM that the ALSA back end can open.  Everything that
// touches the radio or the Bluetooth daemon goes through bluetoothctl, which is
// why every MAC address is validated before it reaches a command line.
//
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>

#include "bluetooth.h"

static bool m_avail_checked;
static bool m_avail;
static char m_why[96];

static char m_adapter[BT_MAC_LEN];
static bool m_powered;

static int  m_count;
static char m_mac[BT_MAX_DEV][BT_MAC_LEN];
static char m_name[BT_MAX_DEV][BT_NAME_LEN];
static char m_icon[BT_MAX_DEV][BT_ICON_LEN];
static bool m_conn[BT_MAX_DEV];

static char m_active_mac[BT_MAC_LEN];
static char m_active_role[12];

// defined at the bottom of the file, but bt_available() needs it above
static bool run_ok(const char *cmd, char *out, int len);

const char *bt_available_why(void) { return m_why; }

bool bt_available(void) {
    if (m_avail_checked) return m_avail;
    m_avail_checked = true;
    m_why[0] = 0;

    if (access("/usr/bin/bluealsa", X_OK) != 0) {
        snprintf(m_why, sizeof(m_why), "bluealsa is not installed");
        return false;
    }
    if (access("/usr/bin/bluetoothctl", X_OK) != 0) {
        snprintf(m_why, sizeof(m_why), "bluetoothctl is not installed");
        return false;
    }
    // The plugin puts its PCM definition in ALSA's configuration; without it
    // BlueZ has no audio profiles at all and a connect attempt comes back as
    // "br-connection-profile-unavailable".  Checked by looking for the file
    // rather than shelling out: the state is read many times a second.
    if (access("/etc/alsa/conf.d/20-bluealsa.conf", R_OK) != 0 &&
        access("/usr/share/alsa/alsa.conf.d/20-bluealsa.conf", R_OK) != 0) {
        snprintf(m_why, sizeof(m_why), "the bluealsa ALSA plugin is not installed");
        return false;
    }
    m_avail = true;
    return true;
}

int  bt_count(void) { return m_count; }
const char *bt_mac(int i)  { return (i >= 0 && i < m_count) ? m_mac[i]  : ""; }
const char *bt_name(int i) { return (i >= 0 && i < m_count) ? m_name[i] : ""; }
const char *bt_icon(int i) { return (i >= 0 && i < m_count) ? m_icon[i] : ""; }
bool bt_connected(int i)   { return (i >= 0 && i < m_count) ? m_conn[i] : false; }
const char *bt_adapter(void) { return m_adapter; }
bool bt_powered(void) { return m_powered; }
const char *bt_active_mac(void)  { return m_active_mac; }
const char *bt_active_role(void) { return m_active_role; }

const char *bt_active_name(void) {
    for (int i = 0; i < m_count; i++)
        if (strcmp(m_mac[i], m_active_mac) == 0) return m_name[i];
    return "";
}

// The cached flag, refreshed when a command asks for the list.  This is in the
// state, which is sent many times a second, so it must not run anything.
bool bt_active_connected(void) {
    if (!m_active_mac[0]) return false;
    for (int i = 0; i < m_count; i++)
        if (strcmp(m_mac[i], m_active_mac) == 0) return m_conn[i];
    return false;
}

bool bt_alsa_devices(char *play, int playlen, char *cap, int caplen) {
    if (!m_active_mac[0] || !m_active_role[0]) return false;
    bool headset = (strcmp(m_active_role, "headset") == 0);
    if (play && playlen > 0)
        snprintf(play, playlen, "bluealsa:DEV=%s,PROFILE=%s",
                 m_active_mac, headset ? "sco" : "a2dp");
    if (cap && caplen > 0) {
        if (headset)
            snprintf(cap, caplen, "bluealsa:DEV=%s,PROFILE=sco", m_active_mac);
        else
            cap[0] = 0;   // A2DP is playback only: the sound card keeps the mic
    }
    return true;
}

bool bt_valid_mac(const char *s) {
    if (!s || strlen(s) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (s[i] != ':') return false;
        } else if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

bool bt_set_active(const char *mac, const char *role) {
    if (!bt_valid_mac(mac)) return false;
    if (strcmp(role, "speaker") != 0 && strcmp(role, "headset") != 0) return false;
    snprintf(m_active_mac, sizeof(m_active_mac), "%s", mac);
    snprintf(m_active_role, sizeof(m_active_role), "%s", role);
    return true;
}

void bt_clear(void) {
    m_active_mac[0] = 0;
    m_active_role[0] = 0;
}

// ---------------------------------------------------------------- the tools

// Run a command and collect its output.  Nothing here is on a hot path: the
// commands that need it are the ones the user asked for.
static bool run_ok(const char *cmd, char *out, int len) {
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    if (out && len > 0) {
        int n = 0, c;
        while (n < len - 1 && (c = fgetc(p)) != EOF) out[n++] = (char)c;
        out[n] = 0;
    } else {
        char buf[256];
        while (fgets(buf, sizeof(buf), p)) { /* drain */ }
    }
    pclose(p);
    return true;
}

// "Controller DC:A6:32:54:DF:63 (public)" -> the address
static void parse_adapter(const char *out) {
    const char *p = strstr(out, "Controller ");
    if (!p) return;
    p += 11;
    int i = 0;
    while (i < BT_MAC_LEN - 1 && p[i] && !isspace((unsigned char)p[i])) {
        m_adapter[i] = (char)toupper((unsigned char)p[i]);
        i++;
    }
    m_adapter[i] = 0;
}

static void parse_info_icon(int idx) {
    char cmd[128], out[1024];
    snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", m_mac[idx]);
    if (!run_ok(cmd, out, sizeof(out))) return;
    const char *p = strstr(out, "Icon: ");
    if (!p) return;
    p += 6;
    int i = 0;
    while (i < BT_ICON_LEN - 1 && p[i] && p[i] != '\n' && p[i] != '\r') {
        m_icon[idx][i] = p[i];
        i++;
    }
    m_icon[idx][i] = 0;
}

void bt_refresh(void) {
    char out[8192];

    m_count = 0;
    m_adapter[0] = 0;
    m_powered = false;

    if (run_ok("bluetoothctl show 2>/dev/null", out, sizeof(out))) {
        parse_adapter(out);
        m_powered = strstr(out, "Powered: yes") != NULL;
    }

    if (!run_ok("bluetoothctl devices 2>/dev/null", out, sizeof(out))) return;

    for (char *line = strtok(out, "\n"); line != NULL && m_count < BT_MAX_DEV; line = strtok(NULL, "\n")) {
        if (strncmp(line, "Device ", 7) != 0) continue;
        char mac[BT_MAC_LEN];
        int i = 0;
        const char *p = line + 7;
        while (i < BT_MAC_LEN - 1 && *p && !isspace((unsigned char)*p)) mac[i++] = *p++;
        mac[i] = 0;
        if (!bt_valid_mac(mac)) continue;
        while (*p == ' ') p++;
        snprintf(m_mac[m_count], BT_MAC_LEN, "%s", mac);
        snprintf(m_name[m_count], BT_NAME_LEN, "%s", *p ? p : mac);
        m_icon[m_count][0] = 0;
        m_conn[m_count] = false;
        m_count++;
    }

    // Which of them are connected right now
    if (m_count > 0 && run_ok("bluetoothctl devices Connected 2>/dev/null", out, sizeof(out))) {
        for (char *line = strtok(out, "\n"); line != NULL; line = strtok(NULL, "\n")) {
            if (strncmp(line, "Device ", 7) != 0) continue;
            char mac[BT_MAC_LEN];
            int i = 0;
            const char *p = line + 7;
            while (i < BT_MAC_LEN - 1 && *p && !isspace((unsigned char)*p)) mac[i++] = *p++;
            mac[i] = 0;
            for (int d = 0; d < m_count; d++)
                if (strcmp(m_mac[d], mac) == 0) m_conn[d] = true;
        }
    }

    // What kind of device each one is, for the UI: audio-card for a speaker,
    // audio-headset for a headset.
    for (int d = 0; d < m_count; d++) parse_info_icon(d);
}

// Both of these are deliberately fire-and-forget: pairing and connecting can
// take several seconds, and the command thread that asked for them should not
// be held up by it.  The result arrives in the list (and in the audio path).
void bt_connect_async(const char *mac) {
    if (!bt_valid_mac(mac)) return;
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "( bluetoothctl trust %s >/dev/null 2>&1; bluetoothctl connect %s >/dev/null 2>&1 ) &",
             mac, mac);
    if (system(cmd) != 0) { /* the shell would not start; nothing else to try */ }
}

bool bt_connect_sync(const char *mac, int timeout_s) {
    if (!bt_valid_mac(mac)) return false;
    char cmd[256], out[1024];
    snprintf(cmd, sizeof(cmd), "timeout %d bluetoothctl connect %s >/dev/null 2>&1",
             timeout_s, mac);
    system(cmd);   // the exit status of timeout says nothing useful here
    snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", mac);
    if (!run_ok(cmd, out, sizeof(out))) return false;
    return strstr(out, "Connected: yes") != NULL;
}

void bt_scan_async(void) {
    if (system("bluetoothctl --timeout 20 scan on >/dev/null 2>&1 &") != 0) { /* as above */ }
}
