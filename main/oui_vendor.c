#include "oui_vendor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#define OUI_MALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#else
#define OUI_MALLOC(n) malloc((n))
#endif

#define OUI_HEADER_SIZE 16
#define OUI_OUI_LEN 3
#define OUI_INDEX_ENTRY_SIZE 6

static uint8_t *s_buf = NULL;     // whole file in one heap buffer
static size_t s_size = 0;         // file size in bytes
static uint32_t s_count = 0;      // index record_count
static uint32_t s_names_off = 0;  // file offset of the name table

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t rd_u24(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

void oui_vendor_unload(void) {
    free(s_buf);
    s_buf = NULL;
    s_size = 0;
    s_count = 0;
    s_names_off = 0;
}

bool oui_vendor_load(const char *path) {
    oui_vendor_unload();
    if (!path) {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    bool ok = false;
    long fsize = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        fsize = ftell(f);
    }
    if (fsize < OUI_HEADER_SIZE || fseek(f, 0, SEEK_SET) != 0) {
        goto done;
    }

    s_buf = (uint8_t *)OUI_MALLOC((size_t)fsize);
    if (!s_buf) {
        goto done;
    }
    if (fread(s_buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        goto done;
    }
    s_size = (size_t)fsize;

    // Validate the header.
    if (memcmp(s_buf, "JVND", 4) != 0 || s_buf[4] != 2 ||
        s_buf[5] != OUI_OUI_LEN || s_buf[6] != OUI_INDEX_ENTRY_SIZE) {
        goto done;
    }
    s_count = rd_u32(s_buf + 8);
    s_names_off = rd_u32(s_buf + 12);

    // The index must fit between the header and the name table, and the name
    // table must start inside the file.
    uint64_t index_end = (uint64_t)OUI_HEADER_SIZE +
                         (uint64_t)s_count * OUI_INDEX_ENTRY_SIZE;
    if (s_names_off > s_size || index_end > s_names_off) {
        goto done;
    }

    ok = true;

done:
    fclose(f);
    if (!ok) {
        oui_vendor_unload();
    }
    return ok;
}

bool oui_vendor_is_loaded(void) {
    return s_buf != NULL;
}

bool oui_vendor_lookup_mac(const uint8_t mac[6], char *out, size_t out_sz) {
    if (out_sz > 0) {
        out[0] = '\0';
    }
    if (!s_buf || !mac) {
        return false;
    }

    for (uint32_t i = 0; i < s_count; i++) {
        const uint8_t *e = s_buf + OUI_HEADER_SIZE + (size_t)i * OUI_INDEX_ENTRY_SIZE;
        if (e[0] != mac[0] || e[1] != mac[1] || e[2] != mac[2]) {
            continue;
        }
        // First match wins; bounds-check the name read, treat bad as miss.
        uint64_t noff = (uint64_t)s_names_off + rd_u24(e + 3);
        if (noff + 1 > s_size) {
            return false;
        }
        uint8_t len = s_buf[noff];
        // Spec says name length is 1..255; a zero length or an out-of-bounds
        // read is treated as a miss.
        if (len == 0 || noff + 1 + (uint64_t)len > s_size) {
            return false;
        }
        if (out_sz == 0) {
            return true;
        }
        size_t copy = len < out_sz - 1 ? len : out_sz - 1;
        memcpy(out, s_buf + noff + 1, copy);
        out[copy] = '\0';
        return true;
    }
    return false;
}

static bool hex_nibble(char c, uint8_t *v) {
    if (c >= '0' && c <= '9') {
        *v = (uint8_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
        *v = (uint8_t)(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
        *v = (uint8_t)(c - 'A' + 10);
    } else {
        return false;
    }
    return true;
}

bool oui_vendor_lookup_str(const char *mac_str, char *out, size_t out_sz) {
    if (out_sz > 0) {
        out[0] = '\0';
    }
    if (!mac_str) {
        return false;
    }

    uint8_t mac[6];
    const char *p = mac_str;
    for (int i = 0; i < 6; i++) {
        uint8_t hi, lo;
        if (!hex_nibble(p[0], &hi) || !hex_nibble(p[1], &lo)) {
            return false;
        }
        mac[i] = (uint8_t)((hi << 4) | lo);
        p += 2;
        if (i < 5) {
            if (*p != ':' && *p != '-') {
                return false;
            }
            p++;
        }
    }
    if (*p != '\0') {
        return false;
    }
    return oui_vendor_lookup_mac(mac, out, out_sz);
}
