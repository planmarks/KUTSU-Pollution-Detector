#include "identity.h"
#include "config.h"            // NVS_NAMESPACE
#include "esp_mac.h"
#include <Preferences.h>
#include <esp_random.h>

static uint8_t s_mac[6]  = {0};
static char    s_suffix[7]  = "000000";
static char    s_macstr[18] = "00:00:00:00:00:00";
static char    s_host[16]   = "kutsu-000000";
static char    s_creature[32] = "Kutsu-000000";   // auto creature name (NVS "cname")
static char    s_name[32]     = "Kutsu-000000";   // resolved display name (custom > creature)

// A small, friendly, SFW creature list. The chosen one is fixed at first boot.
static const char* const CREATURES[] = {
  "Otter","Axolotl","Pangolin","Fennec","Narwhal","Quokka","Tapir","Lynx","Puffin",
  "Gecko","Marten","Ibex","Capybara","Wombat","Okapi","Civet","Dingo","Serval",
  "Caracal","Numbat","Kakapo","Bittern","Macaw","Lemur","Mongoose","Meerkat","Badger",
  "Heron","Osprey","Falcon","Raven","Magpie","Newt","Salamander","Toad","Mantis",
  "Beetle","Cricket","Firefly","Moth","Urchin","Seahorse","Cuttle","Manta","Koi",
};
static const int N_CREATURES = sizeof(CREATURES) / sizeof(CREATURES[0]);

static void resolveName() {                        // custom if set, else creature
  Preferences p; p.begin(NVS_NAMESPACE, true);
  String custom = p.getString("name", "");
  p.end();
  const char* use = custom.length() ? custom.c_str() : s_creature;
  snprintf(s_name, sizeof s_name, "%s", use);
}

void identityInit() {
  // ESP_MAC_WIFI_STA is the canonical "base" MAC of the module; stable + unique.
  if (esp_read_mac(s_mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    uint64_t m = ESP.getEfuseMac();
    for (int i = 0; i < 6; i++) s_mac[i] = (uint8_t)(m >> (8 * i));
  }
  snprintf(s_suffix, sizeof s_suffix, "%02X%02X%02X", s_mac[3], s_mac[4], s_mac[5]);
  snprintf(s_macstr, sizeof s_macstr, "%02X:%02X:%02X:%02X:%02X:%02X",
           s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
  snprintf(s_host,   sizeof s_host,   "kutsu-%02x%02x%02x", s_mac[3], s_mac[4], s_mac[5]);

  // Creature name: generated once and persisted (survives reflash; new only on erase).
  Preferences p; p.begin(NVS_NAMESPACE, false);
  String cname = p.getString("cname", "");
  if (!cname.length()) {
    const char* c = CREATURES[esp_random() % N_CREATURES];
    snprintf(s_creature, sizeof s_creature, "%s-%02X", c, s_mac[5]);   // e.g. "Otter-C3"
    p.putString("cname", s_creature);
  } else {
    snprintf(s_creature, sizeof s_creature, "%s", cname.c_str());
  }
  p.end();

  resolveName();
}

void kutsuSetName(const char* name) {
  Preferences p; p.begin(NVS_NAMESPACE, false);
  if (name && name[0]) p.putString("name", String(name).substring(0, 31));
  else                 p.remove("name");           // empty -> revert to the creature name
  p.end();
  resolveName();
}

const char*    kutsuSuffix()       { return s_suffix; }
const char*    kutsuName()         { return s_name;   }
const char*    kutsuCreatureName() { return s_creature; }
const char*    kutsuMacStr()       { return s_macstr; }
const char*    kutsuHostname()     { return s_host;   }
const uint8_t* kutsuMacBytes()     { return s_mac;    }
