#pragma once
#include <stdint.h>
#include "netlist.h"

// ─── ROZHRANÍ ZDROJE DAT ─────────────────────────────────────────────────────
//
// Tohle je ten šev, kvůli kterému celý projekt takhle dělíme. Zdroj se dá
// vyměnit (ScanSource ↔ PromiscSource) a displej ani sériový výpis o tom
// nevědí.
//
// Železné pravidlo: zdroj nikdy nic netiskne. Ani na Serial, ani na displej.
class NetSource {
public:
  virtual ~NetSource() {}

  virtual void begin() = 0;

  // Volá se v každém loop(). NIKDY NEBLOKUJE.
  // Vrací true právě v tom průchodu, kdy jsou data hotová k vydání.
  virtual bool poll() = 0;

  // Data ke čtení. Ten const je záměrný: seznam vlastní zdroj, nikdo jiný do
  // něj nesmí zapisovat.
  virtual const NetList& nets() const = 0;

  // Krátký štítek o stavu zdroje pro hlavičku displeje.
  // ScanSource vrací "*" nebo "", PromiscSource "P7" podle kanálu.
  //
  // KOMPROMIS, a chci ho mít napsaný celý, ne jen tu lepší půlku:
  //
  //   Zisk — view přestane vědět, co znamená "busy". Dřív tady byl
  //   bool busy() a view_oled si sám překládal true na hvězdičku, čímž znal
  //   sémantiku skenu. Teď jen vytiskne řetězec, který mu zdroj podá, a je
  //   mu jedno, co je za ním.
  //
  //   Ztráta — zdroj naopak začal vyrábět text pro displej a musí vědět, že
  //   se musí vejít do pár znaků. To do zdroje dat nepatří; je to vazba
  //   opačným směrem, jen tenčí.
  //
  //   Čistší varianta by byla, aby zdroj vydal strukturovaný stav (režim jako
  //   enum, číslo kanálu) a view si ho zformátoval sám. Jenže tím by view
  //   zase muselo znát pojem "kanál" a "promiskuitní režim", tedy přesně tu
  //   sémantiku zdroje, které jsme se chtěli zbavit. Vybráno tohle jako menší
  //   zlo, ne jako čisté řešení.
  //
  // Ukazatel musí mířit do paměti, kterou vlastní zdroj a která volání
  // přežije — literál nebo členská proměnná. Nikdy ne lokální buffer.
  virtual const char* status() const = 0;

  // Delší diagnostická poznámka pro sériovou linku, nebo "" když není co
  // hlásit. Zahozené rámce, přeplněný roster a podobně.
  //
  // Tohle NENÍ výjimka ze vzoru výš, je to ten samý vzor podruhé: zdroj vydá
  // krátký text, view neřeší, co v něm je, a jen ho vytiskne. Jeden vzor
  // použitý dvakrát je čistější než jeden vzor plus výjimka — kdyby přibyl
  // třetí takový údaj, patří sem stejnou cestou, ne jako další parametr.
  //
  // Proč to vůbec musí ven ze zdroje: počet zahozených rámců je číslo o tom,
  // co nástroj NEVIDĚL. Kdyby zůstalo jen v paměti, byla by to ta samá tichá
  // díra, jakou zavírá seen() u useknutého seznamu a "+" u přeplněného
  // rosteru. Diagnostika, kterou nikdo nevidí, je stejně dobrá jako žádná.
  //
  // Platí pro ni to samé pravidlo o životnosti ukazatele.
  virtual const char* diagnostics() const = 0;

  // Jak dlouho trval poslední sběr dat [ms]. Pro ladicí výpis.
  virtual uint32_t lastCollectMs() const = 0;
};
