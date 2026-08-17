#include <Arduino.h>
#include "netlist.h"

// Pozor: v .cpp souborech Arduino IDE nedělá tu magii, co dělá v .ino —
// negeneruje prototypy funkcí a neincluduje Arduino.h automaticky. Proto je
// ten #include výš potřeba napsat ručně.

void NetList::clear() {
  count_ = 0;
  seen_  = 0;
}

const NetInfo& NetList::at(uint8_t i) const {
  if (i >= count_) i = (count_ > 0) ? (uint8_t)(count_ - 1) : 0;
  return items_[i];
}

bool NetList::add(const NetInfo& n) {
  // Počítá se KAŽDÁ nabídnutá síť, i ta, která se za chvíli zahodí. Právě
  // rozdíl mezi seen_ a count_ je ta informace, kvůli které to tady je —
  // aby šlo poznat, že hlavička ukazuje jen výřez.
  //
  // Strop je proti přetečení: v promiskuitním režimu se do stejného seznamu
  // bude přidávat dlouhodobě, ne jen 255 sítí z jednoho skenu.
  if (seen_ < 0xFFFF) seen_++;

  // Najdi první uloženou síť, která je slabší než ta nová — tam nová patří.
  // Podmínka je >=, ne >, takže sítě se stejným RSSI si drží pořadí, v jakém
  // přišly (stabilní řazení) a nepřeskakují si každý sken.
  uint8_t pos = 0;
  while (pos < count_ && items_[pos].rssi >= n.rssi) pos++;

  // Pole je plné a nová síť je slabší než všechny uložené → zahodíme ji.
  if (pos >= MAX_NETS) return false;

  // Index posledního prvku, který po vložení ještě existuje. Když je pole
  // plné, je to MAX_NETS-1 a nejslabší síť tím pádem vypadne z konce.
  const uint8_t last = (count_ < MAX_NETS) ? count_ : (uint8_t)(MAX_NETS - 1);

  // Uvolni místo na pozici pos posunem vyšších prvků o jedno dál.
  for (uint8_t i = last; i > pos; i--) items_[i] = items_[i - 1];

  items_[pos] = n;
  if (count_ < MAX_NETS) count_++;
  return true;
}
