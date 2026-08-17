#pragma once
#include <stdint.h>
#include "netlist.h"

// ─── ROZHRANÍ ZDROJE DAT ─────────────────────────────────────────────────────
//
// Tohle je ten šev, kvůli kterému celý projekt takhle dělíme. Až přijde
// promiskuitní režim, napíšeš PromiscSource : public NetSource a v .ino
// vyměníš jeden řádek. Displej ani sériový výpis o tom nebudou vědět.
//
// Proč abstraktní třída a ne jen konvence: cena je 4 B na vtable pointer a
// jedna indirekce na volání. Za to ti kompilátor ohlídá, že nový zdroj
// rozhraní opravdu splnil, a .ino může držet NetSource* a nezajímat se,
// co je za ním.
//
// Železné pravidlo: zdroj nikdy nic netiskne. Ani na Serial, ani na displej.
class NetSource {
public:
  virtual ~NetSource() {}

  virtual void begin() = 0;

  // Volá se v každém loop(). NIKDY NEBLOKUJE.
  // Vrací true právě v tom jednom průchodu, kdy dorazila nová data — to je
  // pro .ino signál "překresli a vypiš".
  virtual bool poll() = 0;

  // Data ke čtení. Ten const je záměrný: seznam vlastní zdroj, nikdo jiný do
  // něj nesmí zapisovat.
  //
  // Proč seznam vlastní zdroj a nevyplňuje cizí: promiskuitní režim data
  // hromadí v čase, nevyrábí je naráz. Takhle stejné rozhraní posadí obojí.
  virtual const NetList& nets() const = 0;

  // Sbírá se právě teď? Jediná věc, kterou displej ví o vnitřku zdroje —
  // aby mohl v hlavičce ukázat, že se něco děje.
  virtual bool busy() const = 0;

  // Jak dlouho trval poslední sběr dat [ms]. Pro ladicí výpis.
  virtual uint32_t lastCollectMs() const = 0;
};
