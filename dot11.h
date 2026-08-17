#pragma once
#include <stdint.h>
#include "netlist.h"

// ─── ROZBOR RÁMCŮ 802.11 ─────────────────────────────────────────────────────
//
// Čisté funkce nad bajty z éteru. Žádný stav, žádná halda, žádný výstup.
// Volají se z callbacku WiFi tasku, takže musí být krátké a mít shora
// omezený počet kroků i na poškozeném rámci.
//
// Proč zvlášť od promiscsource: rozbor bajtů je jiná starost než fronta
// a stavový automat. A při detekci deautentizace se sem bude přidávat, ne
// do zdroje.
//
// VŠECHNO, co sem přijde, jsou data od cizího zařízení. Každé čtení musí být
// omezené délkou, každá délka uvnitř rámce je taky jen bajt z éteru.
namespace dot11 {

// Frame Control, typy a podtypy
static const uint8_t TYPE_MGMT          = 0;
static const uint8_t SUBTYPE_PROBE_RESP = 5;
static const uint8_t SUBTYPE_BEACON     = 8;
static const uint8_t SUBTYPE_DISASSOC   = 10;
static const uint8_t SUBTYPE_DEAUTH     = 12;

// Nejkratší management rámec, který má smysl: hlavička 24 B + FCS 4 B.
static const uint16_t MIN_MGMT_FRAME = 28;

// Nejkratší beacon / probe response: hlavička 24 B + pevná část těla 12 B
// (timestamp 8 + interval 2 + capability 2) + FCS 4 B.
static const uint16_t MIN_BEACON_FRAME = 40;

// Nejkratší deauth / disassoc: hlavička 24 B + reason code 2 B + FCS 4 B.
static const uint16_t MIN_DEAUTH_FRAME = 30;

// Frame Control jsou první dva bajty rámce. Bity se číslují od nejnižšího
// bitu prvního bajtu:
//
//   bajt 0:  7 6 5 4 3 2 1 0
//            └──┬──┘ └┬┘ └┬┘
//            subtype type verze
//
// Volající musí mít ověřeno, že rámec má aspoň MIN_MGMT_FRAME bajtů.
inline uint8_t frameType(const uint8_t* f)    { return (uint8_t)((f[0] >> 2) & 0x03); }
inline uint8_t frameSubtype(const uint8_t* f) { return (uint8_t)((f[0] >> 4) & 0x0F); }

// Rozebere beacon nebo probe response a naplní bssid, ssid, channel a auth.
// rssi a lastSeenMs si doplní volající, ty v rámci nejsou.
//
// len je sig_len z rx_ctrl, tedy délka VČETNĚ čtyřbajtového FCS na konci.
// Funkce si sama ověří, že je dost dlouhá, ještě než od ní začne odečítat.
//
// Vrací false, když je rámec kratší, než standard dovoluje. Poškozený vnitřek
// (nesmyslná délka prvku) není chyba návratové hodnoty — procházení se
// v takovém místě zastaví a použije se to, co se stihlo přečíst; co se
// nepodařilo určit, skončí jako AUTH_OTHER.
bool parseBeacon(const uint8_t* f, uint16_t len, NetInfo& out);

// Co se dá z deauth / disassoc rámce vyčíst.
//
// Ani jedno z toho není důkaz o odesílateli. MAC adresa se u těchhle rámců
// podvrhuje — je to samotný princip toho, jak se sítě ruší. Struktura tedy
// popisuje, ČÍM JMÉNEM je rámec podepsán, ne kdo ho poslal.
struct DeauthInfo {
  uint8_t  bssid[6];    // Address 3 — identifikuje síť bez ohledu na směr
  uint16_t reason;      // reason code z těla rámce
  bool     broadcast;   // Address 1 == ff:ff:ff:ff:ff:ff → odpojit všechny
  bool     signedAsAp;  // Address 2 == Address 3, tedy "podepsáno jako AP"
};

// Rozebere deauth nebo disassoc rámec. Vrací false, když je kratší, než
// standard dovoluje. Volající si musí sám ověřit, že subtype sedí.
bool parseDeauth(const uint8_t* f, uint16_t len, DeauthInfo& out);

// Jméno reason kódu pro ladicí výpis. Vrací "?" u kódů, které neznáme.
//
// Hodnoty ověřeny proti esp_wifi_types_generic.h:113–165 v core 3.3.11, ale
// tabulka je vlastní: kdyby se braly konstanty z IDF, musel by tenhle překlad
// includovat WiFi.h a tahal by ho pak i do výstupních modulů.
const char* reasonName(uint16_t reason);

}  // namespace dot11
