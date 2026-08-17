#include <Arduino.h>
#include "scansource.h"

// Arduino IDE překládá VŠECHNY .cpp ve složce sketche bez ohledu na to, jaký
// zdroj je zvolený v config.h. Bez tohohle obalu by se kód skenu překládal
// do flash i v promiskuitním režimu, kde se nikdy nezavolá.
#if NET_SOURCE == NET_SOURCE_SCAN

#include <WiFi.h>
#include <string.h>

// ─── Překlad zabezpečení z esp_wifi na náš AuthKind ──────────────────────────
//
// Tohle je jediné místo v projektu, kde se objeví wifi_auth_mode_t. Díky tomu
// nemusí view_oled.cpp ani view_serial.cpp includovat WiFi.h.
// Ověřeno proti core 3.3.11, konkrétně proti
//   tools/esp32-libs/3.3.11/include/esp_wifi/include/esp_wifi_types_generic.h
// řádky 87–104. Všechny konstanty použité níž tam existují, takže tady nejsou
// žádné #if guardy — projekt cílí na core 3.x a je to napsané v README.
static AuthKind toAuthKind(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:  return AUTH_OPEN;
    case WIFI_AUTH_OWE:   return AUTH_OWE;
    case WIFI_AUTH_WEP:   return AUTH_WEP;

    // Čisté režimy — síť přijímá právě jednu generaci.
    case WIFI_AUTH_WPA_PSK:  return AUTH_WPA;
    case WIFI_AUTH_WPA2_PSK: return AUTH_WPA2;
    case WIFI_AUTH_WPA3_PSK: return AUTH_WPA3;

    // Smíšené režimy. Nesmí splynout s tou silnější generací — viz komentář
    // u AuthKind v netlist.h.
    case WIFI_AUTH_WPA_WPA2_PSK:  return AUTH_WPA_WPA2;
    case WIFI_AUTH_WPA2_WPA3_PSK: return AUTH_WPA2_WPA3;

    // WPA3_EXT_PSK a WPA3_EXT_PSK_MIXED_MODE jsou obě podle hlavičky
    // deprecated a obě "yield same result as WIFI_AUTH_WPA3_PSK". U toho
    // druhého jméno svádí zařadit ho mezi smíšené, ale to by byl dohad ze
    // jména proti doloženému tvrzení v dokumentaci, takže se držíme hlavičky.
    // Kdyby se ukázalo, že se ta věta týká jen konfigurace AP a ne toho, co
    // hlásí sken, patří MIXED_MODE k AUTH_WPA2_WPA3.
    case WIFI_AUTH_WPA3_EXT_PSK:            return AUTH_WPA3;
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE: return AUTH_WPA3;

    // Enterprise (802.1X). Generace se rozlišuje, protože rozdíl mezi
    // WPA-Enterprise a WPA3-Enterprise je stejně podstatný jako u PSK.
    case WIFI_AUTH_WPA_ENTERPRISE: return AUTH_ENT_WPA;

    // Pozor: WIFI_AUTH_WPA2_ENTERPRISE je v hlavičce definované jako alias
    // (= WIFI_AUTH_ENTERPRISE), tedy stejná hodnota. Uvést obě jména jako
    // dva case by byl duplicitní case a překlad by spadl.
    case WIFI_AUTH_ENTERPRISE: return AUTH_ENT_WPA2;

    // "WPA3-Enterprise Transition Mode" — přijímá i WPA2-Enterprise, takže
    // se podle stejného pravidla hlásí jako ta slabší generace.
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return AUTH_ENT_WPA2;

    case WIFI_AUTH_WPA3_ENTERPRISE: return AUTH_ENT_WPA3;
    case WIFI_AUTH_WPA3_ENT_192:    return AUTH_ENT_WPA3;

    // WAPI_PSK, DPP a cokoliv, co do enumu přibude po 3.3.11.
    default: return AUTH_OTHER;
  }
}

// Sanitizace SSID se přesunula do netlist.cpp jako setSsidSanitized(),
// protože ji potřebují oba zdroje. Bydlí u modelu schválně: díky ní platí
// pravidlo, že co je v NetList, je bezpečné vytisknout — a to je vlastnost
// modelu, ne jednoho zdroje.

void ScanSource::begin() {
  // Režim stanice, ale nikam se nepřipojujeme — jen posloucháme a sondujeme.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  nets_.clear();
  startScan();   // první sken hned, ať displej něco ukáže co nejdřív
}

void ScanSource::startScan() {
  scanStartedAt_ = millis();

  // První true = neblokuj (asynchronní sken).
  // Druhé true = vrať i sítě se skrytým SSID (přijdou s prázdným SSID).
  //
  // scanNetworks() umí i další parametry (passive, ms na kanál, konkrétní
  // kanál), ale jejich pořadí a počet se mezi core 2.x a 3.x měnily. Držíme
  // se prvních dvou, ty jsou stabilní.
  const int16_t r = WiFi.scanNetworks(true, true);

  if (r == WIFI_SCAN_FAILED) {
    // Sken se nepodařilo ani nastartovat. Zpátky do IDLE a zkusíme to za
    // SCAN_PERIOD_MS znovu — automat se tím sám uzdraví.
    state_      = IDLE;
    stateSince_ = millis();
    return;
  }

  state_      = RUNNING;
  stateSince_ = millis();
}

bool ScanSource::poll() {
  if (state_ == IDLE) {
    // Tenhle způsob odečítání (millis() - značka) je jediný správný. Nikdy
    // nepiš millis() > značka + perioda — po 49 dnech millis() přeteče a
    // taková podmínka se rozbije. Rozdíl unsigned čísel přetečení přežije.
    if (millis() - stateSince_ >= SCAN_PERIOD_MS) startScan();
    return false;
  }

  // state_ == RUNNING: ptáme se, jak to jde.
  //   WIFI_SCAN_RUNNING (-1) → pořád běží
  //   WIFI_SCAN_FAILED  (-2) → selhalo
  //   >= 0                   → hotovo, tolik sítí je k vyzvednutí
  const int16_t r = WiFi.scanComplete();

  if (r == WIFI_SCAN_RUNNING) {
    if (millis() - stateSince_ >= SCAN_TIMEOUT_MS) {
      // Pojistka: sken se nikdy neohlásil. Uklidíme a půjdeme na to znovu.
      WiFi.scanDelete();
      state_      = IDLE;
      stateSince_ = millis();
    }
    return false;
  }

  lastCollectMs_ = millis() - scanStartedAt_;
  state_         = IDLE;
  stateSince_    = millis();

  if (r < 0) {           // WIFI_SCAN_FAILED nebo jiná chyba
    WiFi.scanDelete();
    nets_.clear();
    nets_.setSeen(0, false);
    return true;         // "nula sítí" je taky platná informace, ať se to
                         // na displeji i sériovce projeví
  }

  // Gettery WiFi.SSID(), RSSI(), channel() atd. berou index jako uint8_t,
  // takže víc než 255 sítí bychom stejně nepřečetli. Když jich sken vrátil
  // víc, je náš počet jen dolní odhad a musí to být vidět na displeji.
  const bool capped = ((uint16_t)r > 255);
  collect((uint16_t)r, capped);

  // Uvolní seznam, který si drží WiFi stack. Bez tohohle ta data zůstanou
  // ležet v jeho paměti až do dalšího skenu.
  WiFi.scanDelete();
  return true;
}

void ScanSource::collect(uint16_t found, bool capped) {
  nets_.clear();

  const uint32_t now = millis();

  // Bez tohohle stropu by uint8_t čítač na 255 přetekl na nulu a cyklus by
  // běžel navěky.
  if (found > 255) found = 255;

  for (uint8_t i = 0; i < (uint8_t)found; i++) {
    NetInfo n;

    // WiFi.SSID(i) vrací String, tedy haldu. Okamžitě to přefiltrujeme do
    // pevného pole a String zahodíme — nic z haldy nám nepřežije tenhle
    // řádek. (Proto je v NetInfo char[33] a ne String.)
    //
    // Omezení téhle cesty: String je C-string, takže SSID s nulovým bajtem
    // uvnitř je odříznuté ještě dřív, než se k němu dostaneme. Promiskuitní
    // režim tenhle problém nemá, ten čte SSID prvek i s jeho délkou.
    const String ssid = WiFi.SSID(i);
    setSsidSanitized(n, (const uint8_t*)ssid.c_str(), (uint8_t)ssid.length());

    // RSSI přijde jako int32_t. constrain() je pojistka proti tomu, aby se
    // nesmyslná hodnota po přetypování na int8_t nepřeklopila na kladnou.
    //
    // Do lokální proměnné to jde schválně: constrain() je makro, které svůj
    // první argument vloží do výrazu třikrát. Kdyby tam bylo WiFi.RSSI(i)
    // přímo, zavolá se to třikrát za sebou.
    const int32_t rssi = WiFi.RSSI(i);
    n.rssi    = (int8_t)constrain(rssi, -128, 127);
    n.channel = (uint8_t)WiFi.channel(i);
    n.auth    = toAuthKind(WiFi.encryptionType(i));

    // BSSID(i) ukazuje do interního bufferu WiFi stacku, platného jen do
    // scanDelete() — proto se kopíruje, ne ukládá ukazatel.
    const uint8_t* bssid = WiFi.BSSID(i);
    if (bssid) memcpy(n.bssid, bssid, sizeof(n.bssid));
    else       memset(n.bssid, 0, sizeof(n.bssid));

    // Sken vidí všechny sítě naráz, takže je všechny "slyšel" právě teď.
    // Stárnutí tady nic nedělá, ale pole musí být platné v obou režimech.
    n.lastSeenMs = now;

    nets_.add(n);   // vloží se rovnou na správné místo podle RSSI
  }

  // Sken ví přesně, kolik sítí našel — na rozdíl od promiskuitního režimu
  // to nemusí odhadovat z rosteru.
  nets_.setSeen(found, capped);
}

#endif  // NET_SOURCE == NET_SOURCE_SCAN
