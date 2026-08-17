#pragma once
#include <stdint.h>
#include "config.h"

// ─── MODEL ───────────────────────────────────────────────────────────────────
//
// Tenhle soubor nesmí nic vědět ani o WiFi, ani o displeji, ani o sériové
// lince. Drží jen data a jejich uspořádání. Když se to poruší, přestane
// fungovat celý ten princip "vyměň zdroj dat, displeje se nedotkni".

// Vlastní, zjednodušený druh zabezpečení.
//
// Proč nepřenášíme wifi_auth_mode_t z esp_wifi přímo: tím by displej i
// sériový výpis musely includovat WiFi.h, tedy by závisely na zdroji dat.
// Takhle to ScanSource přeloží jednou u sebe a v promiskuitním režimu to
// příště odvodíš z capability bitů beaconu do stejného cíle.
enum AuthKind : uint8_t {
  AUTH_OPEN = 0,   // bez šifrování
  AUTH_WEP,
  AUTH_WPA,
  AUTH_WPA2,
  AUTH_WPA3,
  AUTH_ENTERPRISE, // 802.1X (WPA/WPA2/WPA3-Enterprise)
  AUTH_OTHER       // WAPI, OWE, cokoliv, co neumíme pojmenovat
};

struct NetInfo {
  char     ssid[33];  // 802.11 SSID má max 32 bajtů + terminátor
  uint8_t  bssid[6];  // MAC access pointu; teď se nezobrazuje na displeji,
                      // ale v promiskuitním režimu to bude klíč pro slučování
  int8_t   rssi;      // dBm; reálně -30 až -100, do int8_t se to vejde
  uint8_t  channel;   // 1..14 (WROOM-32D je jen 2,4 GHz)
  AuthKind auth;
};
// sizeof(NetInfo) = 33 + 6 + 1 + 1 + 1 = 42 B. Všechny členy mají zarovnání
// na 1 bajt, takže tam kompilátor nemá kam vložit padding.

// Seznam sítí, trvale seřazený podle síly signálu (nejsilnější první).
//
// Proč pole fixní velikosti a ne String / std::vector: obojí alokuje na haldě.
// Zařízení, které běží hodiny a každých 15 s přepíše celý seznam, si haldu
// roztrhá na fragmenty a jednou mu alokace selže v nejhorší možný moment.
// Fixní pole sedí v .bss, jeho velikost je známá při kompilaci a runtime
// selhat nemůže.
class NetList {
public:
  void clear();

  // Vloží síť na správné místo podle RSSI.
  //
  // Proč se řadí při vkládání a ne zvlášť nějakým sort(): z jednoho místa
  // vypadnou tři věci naráz — (a) řazení, (b) při plném poli prostě vypadne
  // nejslabší síť, žádná zvláštní logika pro přetečení, (c) v promiskuitním
  // režimu ti sítě přijdou po jedné z callbacku, ne jako hotový seznam,
  // takže tenhle tvar API budeš potřebovat i pak.
  //
  // Vrací false, když je pole plné a nová síť je slabší než všechny uložené.
  bool add(const NetInfo& n);

  // Kolik sítí je opravdu uložených a dá se přečíst přes at().
  uint8_t count() const { return count_; }

  // Kolik sítí jsme viděli, VČETNĚ těch, na které se nevešlo místo.
  //
  // Proč to vůbec je: bez tohohle by count() == 24 znamenalo "je tu 24 sítí",
  // i kdyby jich bylo 37. To je tichá falešná jistota — displej by tvrdil
  // něco, co není pravda, a nešlo by to nijak poznat. Rozdíl seen() - count()
  // je přesně to, co se zahodilo.
  uint16_t seen() const { return seen_; }

  // true = MAX_NETS nestačilo a nejslabší sítě vypadly.
  bool truncated() const { return seen_ > count_; }

  // i by mělo být < count(); pro jistotu se to přiskřípne, ať se nikdy
  // nečte za konec pole.
  const NetInfo& at(uint8_t i) const;

private:
  NetInfo  items_[MAX_NETS];
  uint16_t seen_  = 0;
  uint8_t  count_ = 0;
};
// sizeof(NetList) = 1012 B: 24 × 42 pro pole, 2 pro seen_, 1 pro count_
// a 1 bajt paddingu, protože uint16_t chce zarovnání na 2.
