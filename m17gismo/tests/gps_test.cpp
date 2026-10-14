// tests/gps_test.cpp
//
// Checks for the GPS code: Maidenhead conversion, and the M17 META GNSS
// position payload encode/decode round trip (specification 2.0.9 layout).
//
// Build on the Pi from m17gismo/:
//   g++ -O2 -DCODEC2_MOD -Icodec2mod -I. tests/gps_test.cpp gps.cpp -o /tmp/gps_test -lm -lpthread
//
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "m17defines.h"

static int fails = 0;

static void expect_grid(double lat, double lon, const char *want, const char *what) {
    char g[12];
    gps_maidenhead_of(lat, lon, g, sizeof(g));
    bool ok = (strcmp(g, want) == 0);
    if (!ok) fails++;
    printf("  %-8s %.5f, %-10.5f -> %-7s %s   (%s)\n", want, lat, lon, g, ok ? "ok" : "FAIL", what);
}

int main(void) {
    printf("== Maidenhead, known locators ==\n");
    expect_grid(50.803592, -0.441952, "IO90ST", "the GPS fix on the Pi");
    expect_grid(50.8125,   -0.458333, "IO90ST", "centre of IO90ST");
    expect_grid(47.6062,   -122.3321, "CN87UO", "Seattle");
    expect_grid(-33.8688,   151.2093, "QF56OD", "Sydney");
    expect_grid(51.4778,     -0.0015, "IO91XL", "Greenwich");   // recomputed: index 23 = X

    printf("\n== round trip: every field and square centre ==\n");
    int cases = 0;
    for (int f_lo = 0; f_lo < 18; f_lo++) {
        for (int f_la = 0; f_la < 18; f_la++) {
            for (int s_lo = 0; s_lo < 10; s_lo++) {
                for (int s_la = 0; s_la < 10; s_la++) {
                    double lon = -180.0 + f_lo * 20.0 + s_lo * 2.0 + 1.0;   // centre of the square
                    double lat =  -90.0 + f_la * 10.0 + s_la * 1.0 + 0.5;
                    char g[12];
                    gps_maidenhead_of(lat, lon, g, sizeof(g));
                    char want[8];
                    want[0] = 'A' + f_lo; want[1] = 'A' + f_la;
                    want[2] = '0' + s_lo; want[3] = '0' + s_la;
                    want[4] = 0;
                    if (strncmp(g, want, 4) != 0) {
                        if (fails < 12) printf("  FAIL %.5f,%.5f -> %s want %s\n", lat, lon, g, want);
                        fails++;
                    }
                    cases++;
                }
            }
        }
    }
    printf("  %d field/square centres checked\n", cases);

    printf("\n== round trip: subsquares across a field ==\n");
    cases = 0;
    for (int t_lo = 0; t_lo < 24; t_lo++) {
        for (int t_la = 0; t_la < 24; t_la++) {
            // centre of subsquare (S,T) inside IO90
            double lon = -20.0 + 9 * 2.0 + t_lo * (2.0 / 24.0) + (1.0 / 24.0);
            double lat =  50.0 + 0 * 1.0 + t_la * (1.0 / 24.0) + (0.5 / 24.0);
            char g[12], want[8];
            gps_maidenhead_of(lat, lon, g, sizeof(g));
            snprintf(want, sizeof(want), "IO90%c%c", 'A' + t_lo, 'A' + t_la);
            if (strcmp(g, want) != 0) {
                if (fails < 20) printf("  FAIL %.6f,%.6f -> %s want %s\n", lat, lon, g, want);
                fails++;
            }
            cases++;
        }
    }
    printf("  %d subsquares checked\n", cases);

    printf("\n== bearing and distance (hand-checked values) ==\n");
    struct { double la1, lo1, la2, lo2, bearing, tol, dist_lo, dist_hi; const char *what; } bd[] = {
        { 50, 0, 51, 0,   0.0, 0.01, 111.0, 111.4, "due north" },
        { 50, 0, 49, 0, 180.0, 0.01, 111.0, 111.4, "due south" },
        { 50, 0, 50, 1,  89.6, 1.00,   71.0,  72.0, "east along 50N" },
        { 50, 0, 50, -1, 270.4, 1.00,  71.0,  72.0, "west along 50N" },
        { 0, 0, 0, 1,    90.0, 0.20, 111.0, 111.4, "east on the equator" },
        { 50.803592, -0.441952, 51.5074, -0.1278, 15.6, 1.5, 80.0, 83.0, "Ferring to London" },
    };
    for (unsigned i = 0; i < sizeof(bd) / sizeof(bd[0]); i++) {
        double b = gps_bearing(bd[i].la1, bd[i].lo1, bd[i].la2, bd[i].lo2);
        double d = gps_distance_km(bd[i].la1, bd[i].lo1, bd[i].la2, bd[i].lo2);
        double diff = fabs(b - bd[i].bearing);
        if (diff > 180.0) diff = 360.0 - diff;
        bool ok = (diff <= bd[i].tol) && (d >= bd[i].dist_lo) && (d <= bd[i].dist_hi);
        if (!ok) fails++;
        printf("  %-20s bearing %6.1f (want %5.1f)  %6.1f km (want %.0f-%.0f)  %s\n",
               bd[i].what, b, bd[i].bearing, d, bd[i].dist_lo, bd[i].dist_hi, ok ? "ok" : "FAIL");
    }

    printf("\n== M17 META GNSS payload ==\n");
    GpsMsg in, out;
    memset(&in, 0, sizeof(in));
    in.valid = true;
    in.lat = 50.803592;
    in.lon = -0.441952;
    in.alt = 27;                       // metres
    in.course = 143;
    in.speed_kmh = 12.5;
    in.hdop = 1.2f;
    in.have_velocity = true;
    in.station_type = GNSS_STATION_FIXED;
    in.validity = GNSS_VALID_POSITION | GNSS_VALID_ALTITUDE | GNSS_VALID_VELOCITY | GNSS_VALID_RADIUS;

    uint8_t meta[14];
    gps_encode(meta, &in);
    printf("  META  = ");
    for (int i = 0; i < 14; i++) printf("%02X", meta[i]);
    printf("\n");

    gps_decode(meta, &out);
    printf("  lat  %.6f (want %.6f) %s\n", out.lat, in.lat, fabs(out.lat - in.lat) < 0.0005 ? "ok" : "FAIL");
    printf("  lon  %.6f (want %.6f) %s\n", out.lon, in.lon, fabs(out.lon - in.lon) < 0.0010 ? "ok" : "FAIL");
    printf("  alt  %d m (want %d) %s\n", out.alt, in.alt, abs(out.alt - in.alt) <= 1 ? "ok" : "FAIL");
    printf("  spd  %.1f km/h (want %.1f) %s\n", out.speed_kmh, in.speed_kmh, fabs(out.speed_kmh - in.speed_kmh) <= 0.5 ? "ok" : "FAIL");
    printf("  crs  %d deg (want %d) %s\n", out.course, in.course, out.course == in.course ? "ok" : "FAIL");
    printf("  validity 0x%X (want 0x%X) %s\n", out.validity, in.validity, out.validity == in.validity ? "ok" : "FAIL");
    if (fabs(out.lat - in.lat) >= 0.0005 || fabs(out.lon - in.lon) >= 0.0010 ||
        abs(out.alt - in.alt) > 1 || fabs(out.speed_kmh - in.speed_kmh) > 0.5 ||
        out.course != in.course || out.validity != in.validity) fails++;

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
