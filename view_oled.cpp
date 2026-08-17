#include <Arduino.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <string.h>
#include <stdio.h>
#include "view_oled.h"

// Zapojení je ověřené, nesahat: CS=5, DC=16, RST=17, HW SPI má SCK=18, MOSI=23.
//
// Pokud bude obraz posunutý nebo rozsypaný, zakomentuj tenhle řádek
// a odkomentuj ten pod ním — displej má potom řadič SSD1306.
static U8G2_SH1106_128X64_NONAME_F_4W_HW_SPI u8g2(U8G2_R0, 5, 16, 17);
// static U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI u8g2(U8G2_R0, 5, 16, 17);

// ─── Rozvržení 128×64 ────────────────────────────────────────────────────────
//
// Font 6x10 je neproporcionální: 6 px na znak, tedy 21 znaků na šířku.
// Proti 6x12 ušetří 2 px na řádek a přesně o to je tady pátý řádek.
//
//   y=8    WiFi 24 AP *          2/5
//   y=10   ────────────────────────────
//   y=21  12 Nazev-sit~      6  -41
//   y=31   - FreeWifi        1  -58
//   y=41   3 <hidden>       11  -67
//   y=51  23 UPC1234567       6  -72
//   y=61   w StaraSitVe~      3 -100     ← poslední řádek končí na y=63
//
// Mezi sloupcem zabezpečení a jménem je celý znak mezery. Bez ní splývalo
// "12" se začátkem SSID a řádek se dal přečíst úplně jinak, než co znamená.
//
// Účaří (baseline) je y, na kterém stojí písmena. Font 6x10 má ascent 7 a
// descent 2, takže řádek zabírá y-7 až y+2. Poslední řádek: 54..63, přesně
// se vejde do 64 px.
static const uint8_t SCR_W     = 128;
static const uint8_t HDR_BASE  = 8;
static const uint8_t SEP_Y     = 10;
static const uint8_t ROW0_BASE = 21;
static const uint8_t ROW_PITCH = 10;
static const uint8_t AUTH_W          = 12;  // sloupec se zabezpečením = 2 znaky
static const uint8_t GAP_AUTH_SSID_W = 6;   // mezera mezi zabezpečením a SSID
static const uint8_t GAP_SSID_NUM_W  = 2;   // mezera mezi SSID a čísly
// Dvě mezery mají vlastní jména schválně: pletly by se, a každá řeší něco
// jiného. Ta první je čitelnost, ta druhá jen aby se text nedotýkal.
//
// Rozpočet řádku:
//   12 (auth) + 6 (mezera) + 66 (SSID) + 2 (mezera) + 42 (čísla) = 128 px
// Na SSID tedy zbývá 11 znaků.
//
// Proč je ta první mezera celý znak a ne dva pixely: bez ní se sloupec
// zabezpečení slil se začátkem jména a "2O2 INTERNET" se čte jako "202
// INTERNET", "12TP-LINK" jako jedno slovo. A protože avail bylo přesně
// 72 px = 12 znaků, srazila by SSID na 11 znaků i mezera o dvou pixelech —
// za tu samou cenu je tedy lepší mezera plná.
//
// Dvouznakový sloupec zabezpečení tak proti jednoznakovému stojí dohromady
// dva znaky jména (13 → 11). Vědomá výměna, viz authText().

// Pozor na diakritiku: font u8g2_font_6x10_tf pokrývá ASCII a Latin-1, ale
// ě š č ř ž ů jsou v Latin-2. Kdybys do drawStr() dal české "žádné", vykreslí
// se prázdná místa. Proto jsou všechny texty na displeji bez háčků.

// Dva znaky zabezpečení, sloupec úplně vlevo — ten se dá přečíst jedním
// pohledem shora dolů, což na konci řádku nejde.
//
//   -   otevřená            1   jen WPA      12  smíšená, přijímá i WPA
//   o   OWE / Enhanced Open 2   jen WPA2     23  smíšená, přijímá i WPA2
//   w   WEP                 3   jen WPA3     ?   nerozpoznáno
//   e1  Enterprise na WPA   e2  Ent. WPA2    e3  Ent. WPA3 včetně 192bit
//
// Proč dva znaky a ne jeden: dvě číslice vedle sebe se čtou jako "přijímá
// obojí" bez legendy, a to je u nástroje, na který se člověk dívá pět vteřin,
// důležitější než ušetřený znak SSID. Zároveň to řeší Enterprise — e1/e2/e3
// neschovává rozdíl mezi WPA-Enterprise a WPA3-Enterprise a nemusí se kvůli
// tomu podhodnocovat WPA3 Suite-B. A rozšiřuje se to samo: další smíšený
// režim se zapíše stejným pravidlem bez vymýšlení nového písmene.
//
// Jednoznakové hodnoty jsou zarovnané doprava mezerou, aby ve sloupci stály
// pod druhou číslicí těch dvouznakových.
//
// Návratová hodnota ukazuje na literál, takže se nikam nekopíruje a nemá
// životnost, kterou by bylo potřeba hlídat.
static const char* authText(AuthKind a) {
  switch (a) {
    case AUTH_OPEN:      return " -";
    case AUTH_OWE:       return " o";
    case AUTH_WEP:       return " w";
    case AUTH_WPA:       return " 1";
    case AUTH_WPA2:      return " 2";
    case AUTH_WPA3:      return " 3";
    case AUTH_WPA_WPA2:  return "12";
    case AUTH_WPA2_WPA3: return "23";
    case AUTH_ENT_WPA:   return "e1";
    case AUTH_ENT_WPA2:  return "e2";
    case AUTH_ENT_WPA3:  return "e3";
    default:             return " ?";
  }
}

// Zkrátí text tak, aby se vešel do maxW pixelů, a poslední zobrazený znak
// nahradí vlnovkou jako značkou "tady to pokračuje".
//
// Proč se šířka měří přes getStrWidth() a nedělí se prostě šestkou: u 6x10 by
// dělení fungovalo, ale takhle to přežije záměnu fontu za proporcionální.
// Cena je nejhůř 32 volání getStrWidth() na řádek, což je jen sčítání šířek.
static void fitToWidth(char* s, int maxW) {
  if ((int)u8g2.getStrWidth(s) <= maxW) return;

  size_t len = strlen(s);
  while (len > 1) {
    s[len - 1] = '~';
    s[len]     = '\0';
    if ((int)u8g2.getStrWidth(s) <= maxW) return;
    len--;
  }
  s[0] = '\0';
}

static void drawHeader(const NetList& nets, uint8_t page, uint8_t pages,
                       const char* status) {
  // Mezera před štítkem se přidá, jen když nějaký štítek je.
  const char* sep = (status && status[0]) ? " " : "";
  if (status == nullptr) status = "";

  // Když se všechny viděné sítě vešly, ukazuje se jedno číslo. Když se něco
  // zahodilo, ukazují se obě — "24/37 AP" čti jako "mám 24 z 37 viděných".
  // Hlavička, která by v tom druhém případě hlásila jen "24 AP", by tvrdila
  // něco, co není pravda, a nešlo by to z displeje nijak poznat.
  //
  // A "24/96+ AP" znamená "mám 24 z ASPOŇ 96" — druhé číslo je jen dolní
  // odhad, protože i počítadlo zdroje narazilo na svůj strop. Bez toho
  // plus by se ta samá vada jen posunula o patro výš a zase by mlčela.
  char left[26];
  if (nets.truncated()) {
    snprintf(left, sizeof(left), "WiFi %u/%u%s AP%s%s",
             (unsigned)nets.count(), (unsigned)nets.seen(),
             nets.seenIsLowerBound() ? "+" : "",
             sep, status);
  } else {
    snprintf(left, sizeof(left), "WiFi %u AP%s%s",
             (unsigned)nets.count(), sep, status);
  }

  // Nejdelší reálná varianta je "WiFi 24/96+ AP P13", tedy 18 znaků = 108 px.
  // Číslo stránky vpravo je nejvýš "5/5" = 18 px, tedy začíná na x=110.
  // Zbývají 2 px — těsné, ale nepřekrývá se.
  //
  // POZOR: kdyby MAX_NETS přesáhlo 99 nebo PROMISC_ROSTER_CAP taky, přibude
  // číslice, hlavička naroste na 114 px a začne se s číslem stránky prát.
  u8g2.drawStr(0, HDR_BASE, left);

  // Číslo stránky zarovnané k pravému okraji.
  char right[12];
  snprintf(right, sizeof(right), "%u/%u", (unsigned)(page + 1), (unsigned)pages);
  u8g2.drawStr(SCR_W - u8g2.getStrWidth(right), HDR_BASE, right);

  u8g2.drawHLine(0, SEP_Y, SCR_W);
}

static void drawRow(uint8_t base, const NetInfo& n) {
  // Sítě, které se týkal náraz deauth rámců, se kreslí v negativu.
  //
  // Inverze stojí NULA PIXELŮ, což je na 128×64 rozhodující — na značku
  // v řádku už místo není. Řádek zabírá base-7 (ascent) až base+2 (descent),
  // tedy přesně ROW_PITCH pixelů.
  //
  // Co ta značka znamená: síť byla v deauth rámcích JMENOVÁNA, tedy je
  // DOTČENÁ. Neznamená, že ty rámce poslala — MAC odesílatele se u deauth
  // podvrhuje a nástroj nemá jak zjistit, kdo je odeslal doopravdy.
  const bool flagged = (n.deauthFlags & DEAUTH_BURST) != 0;
  if (flagged) {
    u8g2.setDrawColor(1);
    u8g2.drawBox(0, (uint8_t)(base - 7), SCR_W, ROW_PITCH);
    u8g2.setDrawColor(0);   // text se od teď kreslí pozadím
  }

  // Číselný blok: kanál na 2 znaky, RSSI na 4 znaky (kvůli "-100").
  // Vždycky 7 znaků = 42 px, takže čísla stojí ve sloupci pod sebou i když
  // jedno má -41 a druhé -100. Kdybys místo toho použil "%u %d", ušetří to
  // až 2 znaky pro SSID, ale čísla se rozjedou do cukrblíku.
  char right[16];
  snprintf(right, sizeof(right), "%2u %4d", (unsigned)n.channel, (int)n.rssi);
  const int rightW = (int)u8g2.getStrWidth(right);
  u8g2.drawStr(SCR_W - rightW, base, right);

  u8g2.drawStr(0, base, authText(n.auth));

  // Kolik pixelů na SSID vlastně zbylo. Počítá se, ne hádá — proto stačilo
  // u dvouznakového sloupce zvednout AUTH_W a přidat mezeru, a layout se
  // přepočítal sám.
  const int avail = (int)SCR_W - (int)AUTH_W - (int)GAP_AUTH_SSID_W
                              - (int)GAP_SSID_NUM_W - rightW;

  char ssid[34];
  // Skryté SSID přijde ze skenu jako prázdný string. Že se z toho stane
  // "<hidden>", je rozhodnutí o zobrazení, takže to patří sem do view a ne
  // do modelu — sériový výpis si to klidně může pojmenovat jinak.
  const char* src = n.ssid[0] ? n.ssid : "<hidden>";
  strncpy(ssid, src, sizeof(ssid) - 1);
  ssid[sizeof(ssid) - 1] = '\0';
  fitToWidth(ssid, avail);

  u8g2.drawStr(AUTH_W + GAP_AUTH_SSID_W, base, ssid);

  if (flagged) u8g2.setDrawColor(1);   // vrátit kreslení do normálu
}

void oledBegin() {
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.clearBuffer();
  u8g2.drawStr(0, HDR_BASE, "WiFi scanner");
  u8g2.drawStr(0, ROW0_BASE, "hledam site...");
  u8g2.sendBuffer();
}

void oledDrawPage(const NetList& nets, uint8_t page, uint8_t pages,
                  const char* status) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  drawHeader(nets, page, pages, status);

  if (nets.count() == 0) {
    u8g2.drawStr(0, ROW0_BASE + ROW_PITCH, "zadne site");
  } else {
    const uint16_t first = (uint16_t)page * ROWS_PER_PAGE;
    for (uint8_t r = 0; r < ROWS_PER_PAGE; r++) {
      const uint16_t idx = first + r;
      if (idx >= nets.count()) break;
      drawRow(ROW0_BASE + r * ROW_PITCH, nets.at((uint8_t)idx));
    }
  }

  // Jediné místo, kde se do displeje opravdu posílá. 1 kB po SPI je asi 1 ms.
  u8g2.sendBuffer();
}
