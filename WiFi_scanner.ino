/*
 * WiFi scanner — ESP32-WROOM-32D + OLED 128x64 (SH1106, hardwarové SPI)
 *
 * Deska v Arduino IDE: "ESP32 Dev Module".
 * Knihovny: U8g2 (externí), WiFi a SPI jsou součástí ESP32 core.
 *
 * Rozdělení souborů:
 *   netlist.*     MODEL   — seznam sítí, netuší o WiFi ani o displeji
 *   netsource.h   ROZHRANÍ zdroje dat (vyměnitelné)
 *   scansource.*  ZDROJ   — asynchronní WiFi.scanNetworks(), nic netiskne
 *   view_oled.*   VÝSTUP  — vlastní u8g2, nic neskenuje
 *   view_serial.* VÝSTUP  — ladicí výpis
 *   config.h              — kapacity a časování na jednom místě
 *
 * Tenhle soubor je jen lepidlo a hodiny: rozhoduje KDY se co stane, ne JAK.
 */

#include "config.h"
#include "netsource.h"
#include "scansource.h"
#include "view_oled.h"
#include "view_serial.h"

// ─── Zdroj dat ───────────────────────────────────────────────────────────────
//
// Až přijde promiskuitní režim, změní se jen tyhle dva řádky a jeden #include.
// view_oled.cpp ani view_serial.cpp se nedotkneš.
static ScanSource scanSource;
static NetSource* source = &scanSource;

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
  source->begin();     // nastartuje první sken; neblokuje
  pageAt = millis();
}

void loop() {
  // dirty je lokální schválně: všechno, co ho může nastavit, se stane před
  // vykreslením ve stejném průchodu.
  bool dirty = false;

  // 1) Sběr dat. poll() nikdy neblokuje a vrátí true jen v tom průchodu, kdy
  //    dorazila nová data.
  if (source->poll()) {
    serialDump(source->nets(), source->lastCollectMs());
    dirty = true;
  }

  const uint8_t pages = pageCount(source->nets().count());

  // Seznam se mohl zkrátit pod aktuální stránku.
  //
  // Pozor, tady stránku NENULUJEME. Kdybychom po každém skenu skočili na
  // první stránku, tak při 5 stránkách × 3 s = 15 s a scan cyklu ~15 s bys
  // poslední stránku prakticky nikdy neuvidel — reset by přišel dřív, než se
  // k ní stránkování dostane. Takhle stránky rotují dál nezávisle na skenech.
  if (page >= pages) page = (uint8_t)(pages - 1);

  // 2) Přepnutí stránky. Rozdíl unsigned čísel, takže přetečení millis()
  //    po 49 dnech tohle přežije.
  if (millis() - pageAt >= PAGE_MS) {
    pageAt = millis();
    page   = (uint8_t)((page + 1) % pages);
    dirty  = true;
  }

  // 3) Indikátor "sbírám" v hlavičce se mění nezávisle na datech i stránkách,
  //    takže si jeho změnu musíme ohlídat zvlášť — jinak by hvězdička zůstala
  //    viset až do dalšího přepnutí stránky.
  static bool wasBusy = false;
  if (source->busy() != wasBusy) {
    wasBusy = source->busy();
    dirty   = true;
  }

  // 4) Vykreslení jen při změně.
  //
  //    Poslat 1 kB buffer po SPI je asi 1 ms, takže kreslit každý průchod by
  //    reálně nebolelo. Jde o disciplínu: až budeš v loop() obsluhovat
  //    jednotlivé rámce, chceš, aby tam displej nedělal nic, když nemusí.
  static bool firstFrame = true;
  if (dirty || firstFrame) {
    oledDrawPage(source->nets(), page, pages, source->busy());
    firstFrame = false;
  }

  // Žádné delay(). To je podmínka toho, aby sem šel promiskuitní režim
  // vůbec dolepit.
}
