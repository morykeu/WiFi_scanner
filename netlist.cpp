#include <Arduino.h>
#include <string.h>
#include "netlist.h"

// Pozor: v .cpp souborech Arduino IDE nedělá tu magii, co dělá v .ino —
// negeneruje prototypy funkcí a neincluduje Arduino.h automaticky. Proto je
// ten #include výš potřeba napsat ručně.

void setSsidSanitized(NetInfo& n, const uint8_t* src, uint8_t len) {
  const size_t cap = sizeof(n.ssid) - 1;   // 32 znaků + místo na terminátor
  size_t o = 0;

  // Každý bajt se mapuje právě na jeden — nic se nezahazuje ani nevkládá.
  while (o < len && o < cap) {
    const uint8_t c = src[o];
    n.ssid[o] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
    o++;
  }
  n.ssid[o] = '\0';
}

void NetList::clear() {
  count_          = 0;
  seen_           = 0;
  seenLowerBound_ = false;
}

void NetList::setSeen(uint16_t seen, bool lowerBound) {
  seen_           = seen;
  seenLowerBound_ = lowerBound;
}

const NetInfo& NetList::at(uint8_t i) const {
  if (i >= count_) i = (count_ > 0) ? (uint8_t)(count_ - 1) : 0;
  return items_[i];
}

void NetList::removeAt(uint8_t i) {
  if (i >= count_) return;
  for (uint8_t k = i; k + 1 < count_; k++) items_[k] = items_[k + 1];
  count_--;
}

bool NetList::add(const NetInfo& n) {
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

bool NetList::upsertByBssid(const NetInfo& n) {
  for (uint8_t i = 0; i < count_; i++) {
    if (memcmp(items_[i].bssid, n.bssid, sizeof(n.bssid)) == 0) {
      // Známá síť. Odebrat a vložit znovu — RSSI se mezi beacony mění, takže
      // se mění i místo v seřazeném seznamu. Po removeAt() je vždycky volno,
      // takže tenhle add() nemůže selhat.
      //
      // deauthFlags se přenese ze starého záznamu, protože v příchozím
      // beaconu je nula. Bez tohohle řádku by příznak nárazu zhasl při
      // každém dalším beaconu té sítě.
      NetInfo merged = n;
      merged.deauthFlags = items_[i].deauthFlags;
      removeAt(i);
      return add(merged);
    }
  }
  return add(n);   // nová síť; může se nevejít, a to je v pořádku
}

void NetList::clearDeauthFlags() {
  for (uint8_t i = 0; i < count_; i++) items_[i].deauthFlags = 0;
}

bool NetList::setDeauthFlag(const uint8_t* bssid) {
  for (uint8_t i = 0; i < count_; i++) {
    if (memcmp(items_[i].bssid, bssid, sizeof(items_[i].bssid)) == 0) {
      items_[i].deauthFlags |= DEAUTH_BURST;
      return true;
    }
  }
  return false;
}

uint8_t NetList::expireOlderThan(uint32_t nowMs, uint32_t maxAgeMs) {
  uint8_t removed = 0;
  uint8_t i = 0;

  while (i < count_) {
    // Rozdíl unsigned čísel, takže přetečení millis() po 49 dnech tohle
    // přežije. Kdyby se psalo lastSeenMs + maxAge < now, rozbilo by se to.
    if (nowMs - items_[i].lastSeenMs >= maxAgeMs) {
      removeAt(i);      // i se nezvyšuje, na jeho místo se posunul další
      removed++;
    } else {
      i++;
    }
  }
  return removed;
}
