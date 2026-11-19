#ifndef OUI_VENDOR_H
#define OUI_VENDOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Load + validate a JVND v2 OUI binary into memory. Returns false if the file
// is missing or invalid. Idempotent-safe: unloads any previously loaded data
// first (even on failure).
bool oui_vendor_load(const char *path);

// Free any loaded data. Safe to call when nothing is loaded.
void oui_vendor_unload(void);

// True when a valid binary is currently loaded.
bool oui_vendor_is_loaded(void);

// Look up the vendor for the OUI = first 3 bytes of `mac` (a 6-byte array).
// On a hit, copies the NUL-terminated name (truncated to out_sz-1) into `out`
// and returns true. On miss / not-loaded, sets out[0]='\0' (when out_sz>0) and
// returns false.
bool oui_vendor_lookup_mac(const uint8_t mac[6], char *out, size_t out_sz);

// Parse "AA:BB:CC:DD:EE:FF" (lowercase or '-' separators accepted) to 6 bytes
// and look up. Returns false if the string is not a valid MAC.
bool oui_vendor_lookup_str(const char *mac_str, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif // OUI_VENDOR_H
