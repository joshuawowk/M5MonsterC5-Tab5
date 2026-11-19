// Host test for the OUI vendor lookup component.
// Build + run:
//   cc -std=c11 -Wall -I main tests/oui_vendor_test.c main/oui_vendor.c
//      -o /tmp/oui_test && /tmp/oui_test
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oui_vendor.h"

#define BIN_PATH "/home/jwowk/.claude/jobs/d0d4b3ab/tmp/oui_wifi.bin"

static int g_failed = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__,    \
                    __LINE__);                                        \
            g_failed = 1;                                             \
        }                                                             \
    } while (0)

// Case-insensitive substring: is `needle` contained in `hay`?
static int ci_contains(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) {
        return 1;
    }
    for (const char *h = hay; *h; h++) {
        size_t i = 0;
        while (i < nl && h[i] &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nl) {
            return 1;
        }
    }
    return 0;
}

static void expect_vendor(const char *mac_str, const char *substr) {
    char out[128];
    bool hit = oui_vendor_lookup_str(mac_str, out, sizeof(out));
    CHECK(hit, mac_str);
    if (hit) {
        CHECK(out[0] != '\0', "vendor name non-empty");
        CHECK(ci_contains(out, substr), substr);
        if (!ci_contains(out, substr)) {
            fprintf(stderr, "  got \"%s\", wanted substring \"%s\"\n", out,
                    substr);
        }
    }
}

int main(void) {
    CHECK(oui_vendor_load(BIN_PATH), "load " BIN_PATH);
    CHECK(oui_vendor_is_loaded(), "is_loaded after load");

    // Known OUIs cross-checked against JanOS oui.txt.
    expect_vendor("00:00:0C:11:22:33", "Cisco");              // 00-00-0C
    expect_vendor("28:6F:B9:00:00:01", "Nokia Shanghai Bell"); // 28-6F-B9
    expect_vendor("08:EA:44:AB:CD:EF", "Extreme Networks");    // 08-EA-44
    expect_vendor("00:00:97:AA:BB:CC", "Dell");                // 00-00-97
    expect_vendor("00:00:B4:11:22:33", "Edimax");              // 00-00-B4

    // Lowercase string form resolves identically to the byte form.
    {
        char via_str[128];
        char via_bytes[128];
        uint8_t mac[6] = {0x00, 0x00, 0x0c, 0x11, 0x22, 0x33};
        bool s = oui_vendor_lookup_str("00:00:0c:11:22:33", via_str,
                                       sizeof(via_str));
        bool b = oui_vendor_lookup_mac(mac, via_bytes, sizeof(via_bytes));
        CHECK(s && b, "lowercase str and byte form both hit");
        CHECK(strcmp(via_str, via_bytes) == 0, "str/byte vendor match");
    }

    // '-' separators are accepted.
    {
        char out[128];
        CHECK(oui_vendor_lookup_str("00-00-0C-AA-BB-CC", out, sizeof(out)),
              "dash-separated MAC");
    }

    // Deliberately-absent OUI returns false and clears out.
    {
        char out[128] = {'x'};
        uint8_t mac[6] = {0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00};
        bool hit = oui_vendor_lookup_mac(mac, out, sizeof(out));
        CHECK(!hit, "FF:FF:FF is a miss");
        CHECK(out[0] == '\0', "miss clears out[0]");
    }

    // Invalid strings return false.
    {
        char out[128];
        CHECK(!oui_vendor_lookup_str("not-a-mac", out, sizeof(out)),
              "invalid string rejected");
        CHECK(!oui_vendor_lookup_str("00:00:0C:11:22", out, sizeof(out)),
              "too-short MAC rejected");
        CHECK(!oui_vendor_lookup_str("00:00:0C:11:22:33:44", out, sizeof(out)),
              "too-long MAC rejected");
        CHECK(!oui_vendor_lookup_str("GG:00:0C:11:22:33", out, sizeof(out)),
              "non-hex MAC rejected");
    }

    oui_vendor_unload();
    CHECK(!oui_vendor_is_loaded(), "not loaded after unload");

    if (g_failed) {
        return 1;
    }
    printf("PASS oui_vendor\n");
    return 0;
}
