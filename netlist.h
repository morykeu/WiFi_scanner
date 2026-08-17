#pragma once
#include <stdint.h>
#include "config.h"

// ─── MODEL ───────────────────────────────────────────────────────────────────
//
// Tenhle soubor nesmí nic vědět ani o WiFi, ani o displeji, ani o sériové
// lince. Drží jen data a jejich uspořádání. Když se to poruší, přestane
// fungovat celý ten princip "vyměň zdroj dat, displeje se nedotkni".

// Řídící pravidlo: u průzkumu zabezpečení se hlásí NEJSLABŠÍ metoda, kterou
// síť přijímá. Síť ve smíšeném režimu WPA/WPA2 pořád obsluhuje i WPA klienty
// a útočník si vybere tu slabší cestu — to, že umí i WPA2, mu nepřekáží.
// Kdyby se taková síť hlásila jako WPA2, nástroj by tvrdil líp, než jak to je.
// Proto mají smíšené režimy vlastní hodnoty a nesplývají s čistými.
enum AuthKind : uint8_t {
  AUTH_OPEN = 0,   // bez šifrování
  AUTH_OWE,        // Enhanced Open — šifrované, ale bez hesla a bez ověření
  AUTH_WEP,
  AUTH_WPA,        // jen WPA
  AUTH_WPA2,       // jen WPA2
  AUTH_WPA3,       // jen WPA3
  AUTH_WPA_WPA2,   // smíšený, přijímá i WPA
  AUTH_WPA2_WPA3,  // smíšený, přijímá i WPA2
  AUTH_ENT_WPA,    // 802.1X na generaci WPA
  AUTH_ENT_WPA2,   // 802.1X na generaci WPA2
  AUTH_ENT_WPA3,   // 802.1X na generaci WPA3, včetně Suite-B 192bit
  AUTH_OTHER       // WAPI, DPP, nerozebraný rámec — cokoliv, co neumíme určit
};

// Bity v NetInfo::secFlags — co beacon říká o ochraně management rámců.
// Plní se z RSN prvku, jde tedy cestou upsertu spolu se zbytkem beaconu.
static const uint8_t SEC_MFPC = 0x01;  // síť ochranu umí
static const uint8_t SEC_MFPR = 0x02;  // síť ji vyžaduje (802.11w)

// Bity v NetInfo::deauthFlags.
//
// Zvláštní bajt schválně, oddělený od secFlags: tenhle se plní ÚPLNĚ JINOU
// cestou (setDeauthFlag) než zbytek struktury, protože deauth rámce nenesou
// nic z toho, co je v beaconu. Kdyby to byl jeden bajt, přepsal by ho každý
// příchozí beacon nulou.
static const uint8_t DEAUTH_BURST = 0x01;  // sítě se týkal náraz deauth rámců

struct NetInfo {
  uint32_t lastSeenMs;   // millis() posledního slyšení; kvůli stárnutí záznamů
  char     ssid[33];     // 802.11 SSID má max 32 bajtů + terminátor
  uint8_t  bssid[6];     // MAC access pointu; v promisk režimu je to klíč
  int8_t   rssi;         // dBm; reálně -30 až -100, do int8_t se to vejde
  uint8_t  channel;      // 1..14 (WROOM-32D je jen 2,4 GHz)
  AuthKind auth;
  uint8_t  secFlags;     // SEC_*  — z beaconu
  uint8_t  deauthFlags;  // DEAUTH_* — mimo cestu beaconu, viz setDeauthFlag()
};
// sizeof(NetInfo) = 48 B: 4 + 33 + 6 + 1 + 1 + 1 + 1 + 1 = 48 přesně.
// Oba příznakové bajty se vešly do zarovnávacího odpadu, který tu byl dřív —
// struktura tedy nevyrostla ani o bajt. lastSeenMs je první proto, aby padding
// nevyšel doprostřed.

// Naplní n.ssid bezpečnou podobou SSID.
//
// SSID je až 32 libovolných bajtů z éteru. Vysílá je cizí zařízení, které
// nemáme pod kontrolou, a nikde není psáno, že to musí být text. Kdokoli si
// může pojmenovat AP tak, aby v názvu byly ANSI escape sekvence — a ty by
// v terminálu, kde čteš sériovku, umožnily přebarvovat výpis, mazat řádky
// nebo posouvat kurzor. Ladicí výstup, kterému nelze věřit, je horší než
// žádný. Na displeji by se bajty nad 0x7E stejně vykreslily jako prázdná
// místa, protože font _tf pokrývá Latin-1 a víc ne.
//
// Proto se tady nechá jen tisknutelné ASCII (0x20–0x7E) a všechno ostatní
// se změní na tečku. Funkce je záměrně součástí modelu, ne jednotlivých
// zdrojů: platí díky ní pravidlo, na které se dá spolehnout — co je
// v NetList, je bezpečné vytisknout.
//
// Prázdné SSID (skrytá síť) zůstane prázdné. Že se z něj stane "<hidden>",
// je rozhodnutí o zobrazení, a to patří do view.
//
// Bere ukazatel a délku, ne C-string, schválně: v beaconu je SSID prvek
// s vlastní délkou a smí obsahovat nulový bajt uvnitř. Přes WiFi.SSID(),
// které vrací String, se k takovému SSID nedostaneme celému — promiskuitní
// režim ano.
void setSsidSanitized(NetInfo& n, const uint8_t* src, uint8_t len);

// Seznam sítí, trvale seřazený podle síly signálu (nejsilnější první).
//
// Proč pole fixní velikosti a ne String / std::vector: obojí alokuje na haldě.
// Zařízení, které běží hodiny a průběžně přepisuje seznam, si haldu roztrhá
// na fragmenty a jednou mu alokace selže v nejhorší možný moment. Fixní pole
// sedí v .bss, jeho velikost je známá při kompilaci a runtime selhat nemůže.
class NetList {
public:
  void clear();

  // Vloží síť na správné místo podle RSSI. Používá ScanSource, který staví
  // seznam pokaždé od nuly.
  //
  // Proč se řadí při vkládání a ne zvlášť nějakým sort(): z jednoho místa
  // vypadnou tři věci naráz — (a) řazení, (b) při plném poli prostě vypadne
  // nejslabší síť, žádná zvláštní logika pro přetečení, (c) API ve tvaru,
  // který sedí i zdroji, jemuž sítě chodí po jedné.
  //
  // Vrací false, když je pole plné a nová síť je slabší než všechny uložené.
  bool add(const NetInfo& n);

  // Aktualizuje síť podle BSSID, nebo ji vloží, když tam ještě není.
  // Používá PromiscSource, kterému sítě přicházejí po jednom beaconu.
  //
  // Existující záznam se odebere a vloží znovu, aby seznam zůstal seřazený —
  // RSSI se mezi beacony mění, takže se mění i pozice.
  bool upsertByBssid(const NetInfo& n);

  // Zhasne DEAUTH_BURST u všech záznamů.
  void clearDeauthFlags();

  // Rozsvítí DEAUTH_BURST u sítě s daným BSSID. Vrací false, když taková
  // síť v seznamu není — třeba proto, že už vypadla stárnutím.
  //
  // Proč to NEJDE cestou upsertByBssid(): ten kopíruje celý NetInfo z beaconu,
  // kde je deauthFlags nula. Příznak by tedy zhasl při každém dalším beaconu
  // té sítě, tedy zhruba desetkrát za sekundu. Jedno pole, jeden zapisovatel.
  bool setDeauthFlag(const uint8_t* bssid);

  // Vyhodí záznamy, které nebyly slyšet déle než maxAgeMs. Vrací počet
  // vyhozených. Sken tohle nepotřebuje (staví seznam znovu), promiskuitní
  // režim bez toho ano — tam by se odstěhovaná síť už nikdy sama neztratila.
  uint8_t expireOlderThan(uint32_t nowMs, uint32_t maxAgeMs);

  // Kolik sítí je opravdu uložených a dá se přečíst přes at().
  uint8_t count() const { return count_; }

  // Kolik sítí zdroj celkem zná, VČETNĚ těch, na které se nevešlo místo.
  //
  // Proč to vůbec je: bez tohohle by count() == 24 znamenalo "je tu 24 sítí",
  // i kdyby jich bylo 37. To je tichá falešná jistota — displej by tvrdil
  // něco, co není pravda, a nešlo by to nijak poznat.
  uint16_t seen() const { return seen_; }

  // true = seen() je jen DOLNÍ ODHAD, skutečné číslo je vyšší a neznáme ho.
  // Nastane, když i počítadlo zdroje narazí na svůj vlastní strop. Displej
  // to musí ukázat, jinak by se ta samá vada jen posunula o patro výš.
  bool seenIsLowerBound() const { return seenLowerBound_; }

  // true = něco se nevešlo a seznam je jen výřez.
  bool truncated() const { return seen_ > count_ || seenLowerBound_; }

  // Zdroj sám ohlásí, kolik sítí zná — každý je totiž počítá jinak.
  // ScanSource ví přesně, kolik jich sken vrátil. PromiscSource to bere
  // z rosteru BSSID, protože jemu chodí tatáž síť pořád dokola a počítat
  // každý beacon by nedávalo smysl.
  void setSeen(uint16_t seen, bool lowerBound);

  // i by mělo být < count(); pro jistotu se to přiskřípne, ať se nikdy
  // nečte za konec pole.
  const NetInfo& at(uint8_t i) const;

private:
  void removeAt(uint8_t i);

  NetInfo  items_[MAX_NETS];
  uint16_t seen_           = 0;
  uint8_t  count_          = 0;
  bool     seenLowerBound_ = false;
};
