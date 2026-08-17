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
//   y=21  2 Nazev-site-d~     6  -41
//   y=31  - FreeWifi          1  -58
//   y=41  3 <hidden>         11  -67
//   y=51  2 UPC1234567        6  -72
//   y=61  w StaraSitVeSk~     3 -100     ← poslední řádek končí na y=63
//
// Účaří (baseline) je y, na kterém stojí písmena. Font 6x10 má ascent 7 a
// descent 2, takže řádek zabírá y-7 až y+2. Poslední řádek: 54..63, přesně
// se vejde do 64 px.
static const uint8_t SCR_W     = 128;
static const uint8_t HDR_BASE  = 8;
static const uint8_t SEP_Y     = 10;
static const uint8_t ROW0_BASE = 21;
static const uint8_t ROW_PITCH = 10;
static const uint8_t AUTH_W    = 6;   // sloupec se znakem zabezpečení = 1 znak
static const uint8_t GAP_W     = 2;   // mezera mezi SSID a číselným blokem

// Pozor na diakritiku: font u8g2_font_6x10_tf pokrývá ASCII a Latin-1, ale
// ě š č ř ž ů jsou v Latin-2. Kdybys do drawStr() dal české "žádné", vykreslí
// se prázdná místa. Proto jsou všechny texty na displeji bez háčků.

// Jeden znak zabezpečení. Sloupec vlevo se dá přečíst jedním pohledem shora
// dolů, což na konci řádku nejde.
static char authChar(AuthKind a) {
  switch (a) {
    case AUTH_OPEN:       return '-';
    case AUTH_WEP:        return 'w';
    case AUTH_WPA:        return '1';
    case AUTH_WPA2:       return '2';
    case AUTH_WPA3:       return '3';
    case AUTH_ENTERPRISE: return 'e';
    default:              return '?';
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

static void drawHeader(const NetList& nets, uint8_t page, uint8_t pages, bool scanning) {
  // Když se všechny viděné sítě vešly, ukazuje se jedno číslo. Když se něco
  // zahodilo, ukazují se obě — "24/37 AP" čti jako "mám 24 z 37 viděných".
  // Hlavička, která by v tom druhém případě hlásila jen "24 AP", by tvrdila
  // něco, co není pravda, a nešlo by to z displeje nijak poznat.
  char left[24];
  if (nets.truncated()) {
    snprintf(left, sizeof(left), "WiFi %u/%u AP%s",
             (unsigned)nets.count(), (unsigned)nets.seen(),
             scanning ? " *" : "");
  } else {
    snprintf(left, sizeof(left), "WiFi %u AP%s",
             (unsigned)nets.count(), scanning ? " *" : "");
  }
  // Nejdelší možná varianta je "WiFi 255/255 AP *", tedy 17 znaků = 102 px.
  // Číslo stránky vpravo začíná nejdřív na x=110, takže se to nepotká.
  u8g2.drawStr(0, HDR_BASE, left);

  // Číslo stránky zarovnané k pravému okraji.
  char right[12];
  snprintf(right, sizeof(right), "%u/%u", (unsigned)(page + 1), (unsigned)pages);
  u8g2.drawStr(SCR_W - u8g2.getStrWidth(right), HDR_BASE, right);

  u8g2.drawHLine(0, SEP_Y, SCR_W);
}

static void drawRow(uint8_t base, const NetInfo& n) {
  // Číselný blok: kanál na 2 znaky, RSSI na 4 znaky (kvůli "-100").
  // Vždycky 7 znaků = 42 px, takže čísla stojí ve sloupci pod sebou i když
  // jedno má -41 a druhé -100. Kdybys místo toho použil "%u %d", ušetří to
  // až 2 znaky pro SSID, ale čísla se rozjedou do cukrblíku.
  char right[16];
  snprintf(right, sizeof(right), "%2u %4d", (unsigned)n.channel, (int)n.rssi);
  const int rightW = (int)u8g2.getStrWidth(right);
  u8g2.drawStr(SCR_W - rightW, base, right);

  const char a[2] = { authChar(n.auth), '\0' };
  u8g2.drawStr(0, base, a);

  // Kolik pixelů na SSID vlastně zbylo. Počítá se, ne hádá — proto se dá
  // formát číselného bloku změnit a layout drží sám.
  const int avail = (int)SCR_W - (int)AUTH_W - (int)GAP_W - rightW;

  char ssid[34];
  // Skryté SSID přijde ze skenu jako prázdný string. Že se z toho stane
  // "<hidden>", je rozhodnutí o zobrazení, takže to patří sem do view a ne
  // do modelu — sériový výpis si to klidně může pojmenovat jinak.
  const char* src = n.ssid[0] ? n.ssid : "<hidden>";
  strncpy(ssid, src, sizeof(ssid) - 1);
  ssid[sizeof(ssid) - 1] = '\0';
  fitToWidth(ssid, avail);

  u8g2.drawStr(AUTH_W, base, ssid);
}

void oledBegin() {
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.clearBuffer();
  u8g2.drawStr(0, HDR_BASE, "WiFi scanner");
  u8g2.drawStr(0, ROW0_BASE, "hledam site...");
  u8g2.sendBuffer();
}

void oledDrawPage(const NetList& nets, uint8_t page, uint8_t pages, bool scanning) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  drawHeader(nets, page, pages, scanning);

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
