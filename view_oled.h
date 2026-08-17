#pragma once
#include <stdint.h>
#include "netlist.h"

// ─── VÝSTUP NA DISPLEJ ───────────────────────────────────────────────────────
//
// Tenhle modul nic neskenuje. Dostane hotový seznam a číslo stránky a nakreslí
// to. Nic víc.

// Kolik řádků sítí se vejde pod hlavičku.
//
// Proč je tahle konstanta tady a ne v config.h: kolik řádků se vejde, je
// vlastnost fontu a rozlišení — to není nastavení, to je fyzika displeje.
// Naopak KDY přepnout stránku je politika a ta patří do .ino. Přesně tenhle
// rozdíl je celé to dělení v malém.
static const uint8_t ROWS_PER_PAGE = 5;

void oledBegin();

// page je od 0, pages je celkový počet stránek (nejmíň 1).
// scanning rozsvítí v hlavičce hvězdičku, že sběr právě běží.
void oledDrawPage(const NetList& nets, uint8_t page, uint8_t pages, bool scanning);
