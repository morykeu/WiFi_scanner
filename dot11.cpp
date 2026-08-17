#include <Arduino.h>
#include <string.h>
#include "dot11.h"

namespace dot11 {

// ─── Rozvržení management rámce ──────────────────────────────────────────────
//
//   offset  0-1    Frame Control
//           2-3    Duration
//           4-9    Address 1  = příjemce (u beaconu ff:ff:ff:ff:ff:ff)
//          10-15   Address 2  = odesílatel
//          16-21   Address 3  = BSSID          ← bereme tenhle
//          22-23   Sequence Control
//   ── tělo beaconu / probe response ──
//          24-31   Timestamp        (8 B)
//          32-33   Beacon Interval  (2 B)
//          34-35   Capability Info  (2 B, little-endian)
//          36+     tagged parameters
static const uint16_t OFF_ADDR1    = 4;
static const uint16_t OFF_ADDR2    = 10;
static const uint16_t OFF_ADDR3    = 16;
static const uint16_t OFF_CAPAB    = 34;
static const uint16_t OFF_IE_START = 36;

// Tělo deauth i disassoc rámce je jediné dvoubajtové číslo hned za hlavičkou.
static const uint16_t OFF_REASON = 24;

// Capability Information, bit 4 = Privacy (síť něčím šifruje).
static const uint16_t CAP_PRIVACY = 0x0010;

// Tagged parameters, které nás zajímají. Zbytek (podporované rychlosti, WPS,
// WMM, HT/VHT schopnosti…) přeskakujeme podle délky.
static const uint8_t TAG_SSID   = 0;
static const uint8_t TAG_DS     = 3;    // DS Parameter Set = kanál AP
static const uint8_t TAG_RSN    = 48;   // 0x30, WPA2/WPA3
static const uint8_t TAG_VENDOR = 221;  // 0xDD

// Suite selektory jsou OUI (3 B) + typ (1 B).
static const uint8_t OUI_RSN[3] = { 0x00, 0x0F, 0xAC };  // 802.11 standard
static const uint8_t OUI_WPA[3] = { 0x00, 0x50, 0xF2 };  // WPA1, vendor

// Co se dá vyčíst z RSN nebo z WPA vendor prvku.
struct SecInfo {
  bool present   = false;  // prvek v rámci vůbec je
  bool akmValid  = false;  // podařilo se přečíst CELÝ seznam AKM
  bool psk       = false;
  bool sae       = false;  // WPA3-Personal
  bool dot1x     = false;  // Enterprise
  bool owe       = false;  // Enhanced Open
  bool suiteB192 = false;  // WPA3-Enterprise 192bit
  bool mfpc      = false;  // umí ochranu management rámců (802.11w)
  bool mfpr      = false;  // vyžaduje ji
};

// Rozebere blok ve tvaru, který má RSN prvek i tělo WPA vendor prvku:
//
//   +0   verze (2 B, LE, = 1)
//   +2   group cipher suite (4 B)
//   +6   počet pairwise suites (2 B, LE) = n
//   +8   n × 4 B pairwise suites
//        počet AKM suites (2 B, LE) = m
//        m × 4 B AKM suites            ← tohle nás zajímá
//        RSN capabilities (2 B, volitelné)
//
// akmValid se nastaví jen tehdy, když se povedlo dojít až za konec seznamu
// AKM. Když se cestou narazí na nesmyslnou délku, zůstane false a volající
// z toho neudělá žádné tvrzení o zabezpečení — radši "neznámé" než odhad.
static void parseSuiteBlock(const uint8_t* d, uint16_t len,
                            const uint8_t* oui, SecInfo& out) {
  out.present = true;

  uint16_t p = 0;
  if ((uint32_t)p + 2u > len) return;
  const uint16_t version = (uint16_t)d[p] | ((uint16_t)d[p + 1] << 8);
  p += 2;
  if (version != 1) return;              // jiná verze, neumíme ji

  if ((uint32_t)p + 4u > len) return;
  p += 4;                                // group cipher suite přeskočit

  if ((uint32_t)p + 2u > len) return;
  const uint16_t pairwiseCount = (uint16_t)d[p] | ((uint16_t)d[p + 1] << 8);
  p += 2;
  // Počet je taky číslo z éteru. Násobení se dělá v uint32_t, aby se samo
  // nepřeteklo a nevyšlo omylem malé — jinak by kontrola prošla neprávem.
  if ((uint32_t)p + 4u * pairwiseCount > len) return;
  p = (uint16_t)(p + 4u * pairwiseCount);

  if ((uint32_t)p + 2u > len) return;
  const uint16_t akmCount = (uint16_t)d[p] | ((uint16_t)d[p + 1] << 8);
  p += 2;
  if ((uint32_t)p + 4u * akmCount > len) return;

  for (uint16_t i = 0; i < akmCount; i++) {
    const uint8_t* s = d + p + 4u * i;
    if (memcmp(s, oui, 3) != 0) continue;   // cizí OUI, nezajímá nás

    switch (s[3]) {
      case 1: case 3: case 5: case 11: out.dot1x     = true; break;  // 802.1X
      case 2: case 6:                  out.psk       = true; break;  // PSK
      case 8: case 9:                  out.sae       = true; break;  // WPA3
      case 12:                         out.suiteB192 = true; break;  // Suite-B
      case 18:                         out.owe       = true; break;  // OWE
      default: break;
    }
  }

  out.akmValid = true;
  p = (uint16_t)(p + 4u * akmCount);

  // RSN Capabilities, 2 B LE, hned za seznamem AKM. Je nepovinné — když
  // chybí, prostě o ochraně management rámců nic nevíme a oba příznaky
  // zůstanou false.
  //
  //   bit 6 = MFPR, síť ochranu VYŽADUJE (802.11w zapnuté natvrdo)
  //   bit 7 = MFPC, síť ji umí
  //
  // Proč to vytahujeme: u hlášení o nárazu deauth rámců to úplně mění, co ten
  // náraz znamená. Proti síti s vynucenou ochranou jsou podvržené deauth
  // rámce neúčinné — klienti je zahodí. My je přesto uvidíme a spočítáme.
  if ((uint32_t)p + 2u <= len) {
    const uint16_t caps = (uint16_t)d[p] | ((uint16_t)d[p + 1] << 8);
    out.mfpr = (caps & 0x0040) != 0;
    out.mfpc = (caps & 0x0080) != 0;
  }
}

// ─── KDE SE PROMISKUITNÍ REŽIM ROZCHÁZÍ SE SKENEM ────────────────────────────
//
// Bez téhle tabulky je "e2" u WPA3-Enterprise tvrzení, které si nikdo nemá jak
// ověřit. Patří do README, ale bydlí tady, protože tady se to rozhoduje.
//
//   případ                    sken      promisk   proč
//   ─────────────────────────────────────────────────────────────────────────
//   WPA3-Ent bez 192bit       e3        e2        AKM je totožné s WPA2-Ent
//                                                 (1 nebo 5). Liší se jen
//                                                 vynucené MFP, které si
//                                                 legálně zapne i WPA2 síť.
//                                                 Rozlišit to = hádat.
//   WPA3-Ent Suite-B 192bit   e3        e3        AKM 12 je jednoznačné.
//   WEP                       w         w         Sken to má od ovladače,
//                                                 my to ODVOZUJEME: v beaconu
//                                                 není prvek, který by WEP
//                                                 hlásil. Poznáme jen
//                                                 "Privacy ano, WPA rodina ne".
//   RSN s nečitelným AKM      správně   ?         Nehádáme.
//   oříznutý/poškozený rámec  správně   ?         Ovladač skládá výsledek
//                                                 z víc pokusů, my z jednoho
//                                                 rámce.
//   WAPI, DPP                 ?         ?         Shoda, oba to neumí.
//   skryté SSID               <hidden>  <hidden>  Ale probe response jméno
//                                                 občas prozradí — promisk
//                                                 tedy může vidět VÍC.
//   SSID s nulovým bajtem     oříznuté  celé      Promisk čte SSID prvek
//                                                 i s jeho délkou, sken
//                                                 dostane jen C-string.
//   sítě na 5 GHz             nevidí    nevidí    WROOM-32D nemá 5GHz rádio.
//
// Převede zjištěné příznaky na AuthKind podle pravidla "nejslabší přijímaná
// metoda". Kde se nedá rozhodnout, vrací AUTH_OTHER — nehádá se.
static AuthKind decideAuth(bool privacy, const SecInfo& rsn, const SecInfo& wpa) {
  // Ani RSN, ani WPA prvek. Buď je síť otevřená, nebo šifruje něčím, co
  // v beaconu nedeklaruje — a to v praxi znamená WEP.
  //
  // Tohle je jediné místo, kde se odvozuje místo čte. WEP se v beaconu
  // neohlašuje žádným prvkem, pozná se jen podle "Privacy ano, WPA rodina ne".
  if (!rsn.present && !wpa.present) {
    return privacy ? AUTH_WEP : AUTH_OPEN;
  }

  // Prvek tam je, ale seznam AKM se nepodařilo přečíst celý.
  if ((rsn.present && !rsn.akmValid) || (wpa.present && !wpa.akmValid)) {
    return AUTH_OTHER;
  }

  // OWE má Privacy bit nastavený, ale žádné heslo ani ověření.
  if (rsn.owe) return AUTH_OWE;

  // ── Enterprise (802.1X) ──
  // Suite-B 192bit je jediná varianta, kterou lze z beaconu určit jako WPA3.
  // WPA3-Enterprise bez 192bit používá stejné AKM jako WPA2-Enterprise a liší
  // se jen vynucenou ochranou management rámců, což si legálně zapne i WPA2
  // síť — rozlišovat podle toho by byl odhad. Hlásí se tedy e2, tedy ta
  // generace, kterou beacon prokazatelně říká.
  if (rsn.suiteB192) return AUTH_ENT_WPA3;
  if (rsn.dot1x || wpa.dot1x) {
    return wpa.dot1x ? AUTH_ENT_WPA : AUTH_ENT_WPA2;   // smíšený → ta slabší
  }

  // ── PSK ──
  // Když je v rámci WPA1 prvek s PSK, síť přijímá WPA bez ohledu na to, co
  // je v RSN. Nejslabší přijímaná metoda je tedy WPA.
  if (wpa.present && wpa.psk) return rsn.present ? AUTH_WPA_WPA2 : AUTH_WPA;
  if (rsn.psk && rsn.sae)     return AUTH_WPA2_WPA3;   // WPA3 transition mode
  if (rsn.sae)                return AUTH_WPA3;
  if (rsn.psk)                return AUTH_WPA2;

  return AUTH_OTHER;
}

// Skrytá síť posílá SSID prvek buď nulové délky, nebo správně dlouhý, ale
// vyplněný samými nulami. Obojí znamená "jméno neřeknu" a musí skončit jako
// prázdné SSID, jinak by z druhého případu sanitizace udělala řádku teček
// a view by místo <hidden> ukázalo "........".
static bool isHiddenSsid(const uint8_t* d, uint8_t len) {
  if (len == 0) return true;
  for (uint8_t i = 0; i < len; i++) {
    if (d[i] != 0x00) return false;
  }
  return true;
}

bool parseBeacon(const uint8_t* f, uint16_t len, NetInfo& out) {
  // POZOR NA PODTEČENÍ. Níž se od len odečítají 4 bajty FCS. len je hodnota
  // z ovladače a je bez znaménka — kdyby přišla menší než 4, odečtení by se
  // podteklo na obrovské číslo a všechny kontroly rozsahu, které se o 'end'
  // opírají, by byly bezcenné: cyklus přes prvky by lezl cizí pamětí
  // s pocitem, že je pořád v mezích. Proto se délka kontroluje PŘED tím, než
  // se od ní začne odečítat. Je to ta samá logika jako u délky prvku
  // v procházení níž, jen o úroveň výš.
  if (len < MIN_BEACON_FRAME) return false;

  const uint16_t end = (uint16_t)(len - 4);   // FCS na konci nás nezajímá

  memcpy(out.bssid, f + OFF_ADDR3, sizeof(out.bssid));

  const uint16_t cap = (uint16_t)f[OFF_CAPAB] | ((uint16_t)f[OFF_CAPAB + 1] << 8);
  const bool privacy = (cap & CAP_PRIVACY) != 0;

  out.ssid[0]     = '\0';  // dokud nenajdeme SSID prvek, bereme jako skrytou
  out.channel     = 0;     // 0 = neřečeno, volající doplní kanál příjmu
  out.rssi        = 0;     // v rámci není, doplní volající z rx_ctrl
  out.lastSeenMs  = 0;     // razítko dá loop(), ne callback
  out.secFlags    = 0;
  out.deauthFlags = 0;     // příznak nárazu jde úplně jinou cestou

  SecInfo rsn, wpa;

  // ── Procházení tagged parameters ──
  // Každý prvek je [tag][délka][data…], poskládané za sebou až do konce rámce.
  uint16_t pos = OFF_IE_START;
  while (pos + 2 <= end) {
    const uint8_t tag  = f[pos];
    const uint8_t ilen = f[pos + 1];

    // Tahle jediná podmínka je celá bezpečnost průchodu. Délka je bajt
    // z éteru: bez téhle kontroly by stačil jeden pokažený beacon a čteme
    // cizí paměť. Když délka nesedí, dál už nevěříme ničemu a končíme —
    // od toho místa je zbytek rámce neinterpretovatelný.
    if ((uint32_t)pos + 2u + ilen > end) break;

    const uint8_t* d = f + pos + 2;

    switch (tag) {
      case TAG_SSID:
        if (!isHiddenSsid(d, ilen)) setSsidSanitized(out, d, ilen);
        break;

      case TAG_DS:
        // Kanál raději odsud než z rx_ctrl: sousední kanály se překrývají,
        // takže AP z kanálu 1 je slyšet i při naladění na 2 nebo 3. Tenhle
        // prvek je to, co o sobě AP samo tvrdí.
        if (ilen >= 1) out.channel = d[0];
        break;

      case TAG_RSN:
        parseSuiteBlock(d, ilen, OUI_RSN, rsn);
        break;

      case TAG_VENDOR:
        // Vendor prvek je OUI (3 B) + typ (1 B) + data. WPA1 je OUI 00:50:F2
        // s typem 1; ostatní vendor prvky (WPS, WMM, výrobci) nás nezajímají.
        if (ilen >= 4 && memcmp(d, OUI_WPA, 3) == 0 && d[3] == 0x01) {
          parseSuiteBlock(d + 4, (uint16_t)(ilen - 4), OUI_WPA, wpa);
        }
        break;

      default:
        break;
    }

    pos = (uint16_t)(pos + 2u + ilen);
  }

  out.auth = decideAuth(privacy, rsn, wpa);

  if (rsn.mfpc) out.secFlags |= SEC_MFPC;
  if (rsn.mfpr) out.secFlags |= SEC_MFPR;

  return true;
}

bool parseDeauth(const uint8_t* f, uint16_t len, DeauthInfo& out) {
  // Ta samá kontrola PŘED odečítáním jako u parseBeacon: len je bez znaménka
  // a přijde z ovladače. Tady se sice od délky nic neodečítá, ale čtou se
  // pevné offsety až do 25, takže minimální délka musí platit stejně tvrdě.
  if (len < MIN_DEAUTH_FRAME) return false;

  // Address 3 je BSSID a identifikuje síť bez ohledu na směr rámce. Kdyby se
  // bral Address 2 (odesílatel), lišil by se podle toho, jestli rámec posílá
  // AP klientovi, nebo klient AP.
  memcpy(out.bssid, f + OFF_ADDR3, sizeof(out.bssid));

  out.reason = (uint16_t)f[OFF_REASON] | ((uint16_t)f[OFF_REASON + 1] << 8);

  // Address 1 je příjemce. Samé jedničky znamenají "všichni klienti naráz",
  // což je podstatně silnější jev než odpojení jednoho klienta.
  out.broadcast = true;
  for (uint8_t i = 0; i < 6; i++) {
    if (f[OFF_ADDR1 + i] != 0xFF) { out.broadcast = false; break; }
  }

  // Odesílatel se podepsal jako samotné AP. Neznamená to, že jím je —
  // podvrhnout MAC je celý princip toho, jak se sítě ruší.
  out.signedAsAp = (memcmp(f + OFF_ADDR2, f + OFF_ADDR3, 6) == 0);

  return true;
}

const char* reasonName(uint16_t reason) {
  switch (reason) {
    case 1:  return "unspecified";
    case 2:  return "prev auth not valid";
    case 3:  return "deauth, station leaving";
    case 4:  return "disassoc, inactivity";
    case 5:  return "AP out of resources";
    case 6:  return "class-2 frame from nonauth STA";
    case 7:  return "class-3 frame from nonassoc STA";
    case 8:  return "disassoc, station leaving";
    case 9:  return "assoc but not authed";
    case 15: return "4-way handshake timeout";
    default: return "?";
  }
}

}  // namespace dot11
