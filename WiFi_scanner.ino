/*
 * WiFi scanner — ESP32-WROOM-32D + OLED 128x64 (SH1106, hardwarové SPI)
 *
 * Deska v Arduino IDE: "ESP32 Dev Module", core 3.x (vyvíjeno na 3.3.11).
 * Knihovny: U8g2 (externí), WiFi a SPI jsou součástí ESP32 core.
 *
 * Rozdělení souborů:
 *   netlist.*      MODEL    — seznam sítí, netuší o WiFi ani o displeji
 *   netsource.h    ROZHRANÍ zdroje dat (vyměnitelné)
 *   scansource.*   ZDROJ    — asynchronní WiFi.scanNetworks()
 *   dot11.*                 — čistý rozbor rámců 802.11, bez stavu a výstupu
 *   promiscsource.* ZDROJ   — promiskuitní poslech: seznam z beaconů
 *                             a počítání deauth / disassoc rámců
 *   view_oled.*    VÝSTUP   — vlastní u8g2, nic neskenuje
 *   view_serial.*  VÝSTUP   — ladicí výpis
 *   config.h                — kapacity, časování a volba zdroje
 *
 * Tenhle soubor je jen lepidlo a hodiny: rozhoduje KDY se co stane, ne JAK.
 */

#include <string.h>
#include "config.h"
#include "netsource.h"
#include "view_oled.h"
#include "view_serial.h"

// ─── Zdroj dat ───────────────────────────────────────────────────────────────
//
// Přepíná se v config.h a je to překladový přepínač, ne běhový: promiskuitní
// režim a scanNetworks() se v rádiu vylučují, protože obojí naráz dělat nejde.
//
// Tohle je celý ten slíbený efekt vyměnitelného zdroje — pod tímhle #if se
// nemění nic jiného. view_oled ani view_serial o existenci dvou zdrojů nevědí.
#if NET_SOURCE == NET_SOURCE_PROMISC
  #include "promiscsource.h"
  static PromiscSource theSource;
#else
  #include "scansource.h"
  static ScanSource theSource;
#endif

static NetSource* source = &theSource;

// ─── Stav stránkování ────────────────────────────────────────────────────────
static uint8_t  page   = 0;
static uint32_t pageAt = 0;   // millis() posledního přepnutí stránky

// Kolik stránek zabere daný počet sítí. Vždycky nejmíň 1, ať se dá zobrazit
// i "zadne site" a hlavička nemluví o stránce 1/0.
static uint8_t pageCount(uint8_t nets) {
  if (nets == 0) return 1;
  return (uint8_t)((nets + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
}

void setup() {
  serialBegin();
  oledBegin();
  source->begin();
  pageAt = millis();
}

void loop() {
  // dirty je lokální schválně: všechno, co ho může nastavit, se stane před
  // vykreslením ve stejném průchodu.
  bool dirty = false;

  // 1) Sběr dat. poll() nikdy neblokuje a vrátí true jen v tom průchodu, kdy
  //    jsou data hotová k vydání — u skenu po dokončení skenu, u promisk
  //    režimu po dokončení celé otočky kanálů.
  if (source->poll()) {
    serialDump(source->nets(), source->lastCollectMs(), source->diagnostics());
    dirty = true;
  }

  const uint8_t pages = pageCount(source->nets().count());

  // Seznam se mohl zkrátit pod aktuální stránku.
  //
  // Pozor, tady stránku NENULUJEME. Kdybychom po každé dávce skočili na
  // první stránku, tak při 5 stránkách × 3 s = 15 s a cyklu ~15 s bys
  // poslední stránku prakticky nikdy neuvidel — reset by přišel dřív, než se
  // k ní stránkování dostane. Takhle stránky rotují dál nezávisle na datech.
  if (page >= pages) page = (uint8_t)(pages - 1);

  // 2) Přepnutí stránky. Rozdíl unsigned čísel, takže přetečení millis()
  //    po 49 dnech tohle přežije.
  if (millis() - pageAt >= PAGE_MS) {
    pageAt = millis();
    page   = (uint8_t)((page + 1) % pages);
    dirty  = true;
  }

  // 3) Štítek zdroje v hlavičce se mění nezávisle na datech i stránkách,
  //    takže si jeho změnu musíme ohlídat zvlášť.
  //
  //    Porovnává se obsah, ne ukazatel: PromiscSource vrací pořád stejný
  //    ukazatel na svůj členský buffer a mění se jen to, co v něm je. Kopie
  //    tady musí být, protože ten buffer nám zdroj kdykoli přepíše.
  //
  //    V promiskuitním režimu se štítek mění při každém přeladění, tedy
  //    čtyřikrát za sekundu. Překreslení stojí asi 1 ms po SPI, takže je to
  //    zanedbatelné — a je to zároveň ten signál "jede to a přelaďuje".
  static char lastStatus[8] = { 0 };
  const char* st = source->status();
  if (strncmp(st, lastStatus, sizeof(lastStatus)) != 0) {
    strncpy(lastStatus, st, sizeof(lastStatus) - 1);
    lastStatus[sizeof(lastStatus) - 1] = '\0';
    dirty = true;
  }

  // 4) Vykreslení jen při změně.
  //
  //    Poslat 1 kB buffer po SPI je asi 1 ms, takže kreslit každý průchod by
  //    reálně nebolelo. Jde o disciplínu: v promiskuitním režimu musí loop()
  //    stíhat vysávat frontu z WiFi tasku a nemá se čím zdržovat.
  static bool firstFrame = true;
  if (dirty || firstFrame) {
    oledDrawPage(source->nets(), page, pages, st);
    firstFrame = false;
  }

  // Žádné delay(). V promiskuitním režimu je to podmínka toho, aby se fronta
  // stihla vysávat dřív, než se naplní.
}
