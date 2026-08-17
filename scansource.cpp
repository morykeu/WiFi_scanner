#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include "scansource.h"

// ─── Překlad zabezpečení z esp_wifi na náš AuthKind ──────────────────────────
//
// Tohle je jediné místo v projektu, kde se objeví wifi_auth_mode_t. Díky tomu
// nemusí view_oled.cpp ani view_serial.cpp includovat WiFi.h.
static AuthKind toAuthKind(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return AUTH_OPEN;
    case WIFI_AUTH_WEP:             return AUTH_WEP;
    case WIFI_AUTH_WPA_PSK:         return AUTH_WPA;
    case WIFI_AUTH_WPA2_PSK:        return AUTH_WPA2;
    case WIFI_AUTH_WPA_WPA2_PSK:    return AUTH_WPA2;  // smíšený režim
    case WIFI_AUTH_WPA2_ENTERPRISE: return AUTH_ENTERPRISE;
    case WIFI_AUTH_WPA3_PSK:        return AUTH_WPA3;
    case WIFI_AUTH_WPA2_WPA3_PSK:   return AUTH_WPA3;  // smíšený režim
    default:                        return AUTH_OTHER;
  }
}

// ─── Sanitizace SSID ─────────────────────────────────────────────────────────
//
// SSID je 32 libovolných bajtů z éteru. Vysílá ho cizí zařízení, které nemáme
// pod kontrolou, a nikde není psáno, že to musí být text. Kdokoli si může
// pojmenovat AP tak, aby v názvu byly ANSI escape sekvence — a ty by
// v terminálu, kde čteš sériovku, umožnily přebarvovat výpis, mazat řádky
// nebo posouvat kurzor. Ladicí výstup, kterému nelze věřit, je horší než
// žádný. Na displeji by se bajty nad 0x7E stejně vykreslily jako prázdná
// místa, protože font _tf pokrývá Latin-1 a víc ne.
//
// Proto se tady, na hranici, kde se data z rádia stávají modelem, nechá jen
// tisknutelné ASCII (0x20–0x7E) a všechno ostatní se změní na tečku. Jednou,
// ne v každém view — aby platilo jednoduché pravidlo, na které se dá spolehnout:
// co je v NetList, je bezpečné vytisknout.
//
// Prázdné SSID (skrytá síť) zůstane prázdné. Že se z něj stane "<hidden>", je
// rozhodnutí o zobrazení, ne o datech, a patří proto do view.
//
// Jedno omezení, které tímhle nespravíme: WiFi.SSID() nám podá String, tedy
// C-string. Když má SSID uvnitř nulový bajt, je zbytek odříznutý už předtím,
// než se k němu dostaneme. Řešit by to šlo jen obejitím Arduino WiFi vrstvy
// a čtením wifi_ap_record_t přímo, což tady za to nestojí.
static void copySanitizedSsid(char* dst, size_t dstSize, const char* src) {
  size_t i = 0;
  // Každý bajt se mapuje právě na jeden, nic se nezahazuje ani nevkládá,
  // takže stačí jeden index pro zdroj i cíl.
  while (src[i] != '\0' && i + 1 < dstSize) {
    const unsigned char c = (unsigned char)src[i];
    dst[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
    i++;
  }
  dst[i] = '\0';
}

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
    return true;         // "nula sítí" je taky platná informace, ať se to
                         // na displeji i sériovce projeví
  }

  collect((uint16_t)r);

  // Uvolní seznam, který si drží WiFi stack. Bez tohohle ta data zůstanou
  // ležet v jeho paměti až do dalšího skenu.
  WiFi.scanDelete();
  return true;
}

void ScanSource::collect(uint16_t found) {
  nets_.clear();

  // Gettery WiFi.SSID(), RSSI(), channel() atd. berou index jako uint8_t,
  // takže víc než 255 sítí bychom stejně nepřečetli. Bez tohohle stropu by
  // uint8_t čítač na 255 přetekl na nulu a cyklus by běžel navěky.
  if (found > 255) found = 255;

  for (uint8_t i = 0; i < (uint8_t)found; i++) {
    NetInfo n;

    // WiFi.SSID(i) vrací String, tedy haldu. Okamžitě to přefiltrujeme do
    // pevného pole a String zahodíme — nic z haldy nám nepřežije tenhle
    // řádek. (Proto je v NetInfo char[33] a ne String.)
    const String ssid = WiFi.SSID(i);
    copySanitizedSsid(n.ssid, sizeof(n.ssid), ssid.c_str());

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

    nets_.add(n);   // vloží se rovnou na správné místo podle RSSI
  }
}
