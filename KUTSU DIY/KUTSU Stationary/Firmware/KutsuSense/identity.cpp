#include "identity.h"
#include "esp_mac.h"

static uint8_t s_mac[6]  = {0};
static char    s_suffix[7]  = "000000";
static char    s_name[16]   = "Kutsu-000000";
static char    s_macstr[18] = "00:00:00:00:00:00";
static char    s_host[16]   = "kutsu-000000";

void identityInit() {
  // ESP_MAC_WIFI_STA is the canonical "base" MAC of the module; it is stable
  // and unique per chip. (The BLE address is derived from this same base.)
  if (esp_read_mac(s_mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    // Extremely unlikely; fall back to the efuse MAC helper so we never advertise
    // as the all-zero default.
    uint64_t m = ESP.getEfuseMac();
    for (int i = 0; i < 6; i++) s_mac[i] = (uint8_t)(m >> (8 * i));
  }
  snprintf(s_suffix, sizeof s_suffix, "%02X%02X%02X", s_mac[3], s_mac[4], s_mac[5]);
  snprintf(s_name,   sizeof s_name,   "Kutsu-%s", s_suffix);
  snprintf(s_macstr, sizeof s_macstr, "%02X:%02X:%02X:%02X:%02X:%02X",
           s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
  snprintf(s_host,   sizeof s_host,   "kutsu-%02x%02x%02x", s_mac[3], s_mac[4], s_mac[5]);
}

const char*    kutsuSuffix()   { return s_suffix; }
const char*    kutsuName()     { return s_name;   }
const char*    kutsuMacStr()   { return s_macstr; }
const char*    kutsuHostname() { return s_host;   }
const uint8_t* kutsuMacBytes() { return s_mac;    }
