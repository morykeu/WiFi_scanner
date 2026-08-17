#pragma once
#include <stdint.h>
#include "netsource.h"

#if NET_SOURCE == NET_SOURCE_PROMISC

// Seznam BSSID, které jsme slyšeli — nezávisle na tom, kolik se jich vejde
// do NetList.
//
// Proč vůbec existuje: v promiskuitním režimu chodí táž síť pořád dokola,
// beacon za beaconem. Kdyby se "kolik sítí známe" počítalo při každém
// vložení jako u skenu, vyšlo by za minutu několik tisíc. Bez rosteru by
// hlavička v promisk režimu tiše přestala hlásit, že se něco nevešlo — a to
// je přesně ta vada, kvůli které to počítadlo vzniklo.
//
// Roster má ale vlastní strop, a když na něj narazí, přestává vědět i on.
// Proto ten příznak overflowed() — displej pak místo "24/96" ukáže "24/96+",
// tedy "aspoň 96". Když nástroj neví, kolik toho neví, musí to říct.
class BssidRoster {
public:
  void clear();

  // Zaznamená, že jsme tuhle síť právě slyšeli.
  void touch(const uint8_t* bssid, uint32_t nowMs);

  void expireOlderThan(uint32_t nowMs, uint32_t maxAgeMs);

  uint16_t count() const { return count_; }

  // true = roster je plný a musel jsme zahodit síť, kterou jsme ještě
  // neznali. count() je od té chvíle jen dolní odhad.
  bool overflowed() const { return overflowed_; }
  void clearOverflow() { overflowed_ = false; }

private:
  struct Entry {
    uint32_t lastSeenMs;   // první kvůli zarovnání, viz NetInfo
    uint8_t  bssid[6];
  };
  // sizeof(Entry) = 12 B (4 + 6, zaokrouhleno na násobek 4).
  // 96 × 12 = 1152 B.
  Entry    items_[PROMISC_ROSTER_CAP];
  uint16_t count_      = 0;
  bool     overflowed_ = false;
};

// Zdroj dat, který místo scanNetworks() poslouchá rádio v promiskuitním
// režimu, čte beacony a probe response a staví z nich stejný NetList.
//
// NIC NEVYSÍLÁ. Volá se jen esp_wifi_set_promiscuous(), _filter(), _rx_cb()
// a esp_wifi_set_channel(). Žádné esp_wifi_80211_tx(), žádné probe requesty,
// žádný aktivní sken.
class PromiscSource : public NetSource {
public:
  void begin() override;
  bool poll() override;

  const NetList& nets() const override { return nets_; }
  const char* status() const override { return status_; }
  const char* diagnostics() const override { return diag_; }
  uint32_t lastCollectMs() const override { return lastCollectMs_; }

private:
  void retune(uint8_t ch);
  void updateStatus();
  void updateDiagnostics();
  void evaluateDeauth(uint32_t now);

  // Pohled loop() na jeden slot tabulky deauth počítadel.
  //
  // Počítadla ve sdílené tabulce jsou MONOTÓNNÍ a loop() je nikdy nenuluje —
  // nulování v souběhu s inkrementem by ten inkrement tiše zahodilo, což je
  // přesně ta chyba, proti které tahle funkce je. Místo toho si loop() drží
  // předchozí hodnotu a odečítá.
  //
  // generation odhalí, že slot byl mezitím přidělen jinému BSSID — pak jsou
  // předchozí hodnoty bezcenné a začíná se od nuly.
  struct SlotView {
    uint32_t generation = 0;
    uint32_t deauth     = 0;
    uint32_t disassoc   = 0;
    uint32_t broadcast  = 0;
    uint32_t burstAtMs  = 0;   // 0 = žádný náraz na kontě
  };

  NetList     nets_;
  BssidRoster roster_;
  SlotView    prev_[DEAUTH_SLOTS];

  uint8_t  channel_        = PROMISC_CHAN_FIRST;
  uint32_t dwellStartedAt_ = 0;
  uint32_t sweepStartedAt_ = 0;
  uint32_t lastCollectMs_  = 0;
  uint32_t lastDrops_      = 0;
  uint32_t lastRecycled_   = 0;

  // Náraz zachycený právě v téhle otočce, -1 = žádný. Podrobnosti se
  // vypisují jen jednou, ne po celou dobu držení příznaku.
  int8_t   newBurst_       = -1;
  uint8_t  burstBssid_[6]  = { 0 };
  uint32_t burstFrames_    = 0;
  uint32_t burstBcast_     = 0;
  uint16_t burstReason_    = 0;
  uint8_t  burstChannel_   = 0;
  bool     burstSignedAsAp_ = false;
  bool     burstOnRecord_  = false;

  // Buffery jsou členské proměnné schválně: ukazatel, který status()
  // a diagnostics() vracejí, musí přežít návrat z volání. Lokální buffer
  // by byl visící ukazatel a kompilátor by na to nemusel upozornit.
  char status_[8] = { 0 };
  char diag_[768] = { 0 };   // hlášení o nárazu má několik řádků, viz .cpp
};

#endif  // NET_SOURCE == NET_SOURCE_PROMISC
