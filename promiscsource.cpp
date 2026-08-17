#include <Arduino.h>
#include "promiscsource.h"

// Arduino IDE překládá VŠECHNY .cpp ve složce sketche bez ohledu na to, jaký
// zdroj je zvolený v config.h. Bez tohohle obalu by se promiskuitní kód
// překládal do flash i v režimu skenu, kde se nikdy nezavolá.
#if NET_SOURCE == NET_SOURCE_PROMISC

#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>
#include <stdio.h>
#include "dot11.h"

// ─── Most mezi WiFi taskem a loop() ──────────────────────────────────────────
//
// esp_wifi_set_promiscuous_rx_cb() bere holý ukazatel na funkci — nemá
// parametr pro uživatelská data. Proto jsou fronta i počítadlo souborové
// statiky. Instance PromiscSource je v projektu právě jedna, takže to je
// v pořádku; kdyby jich mělo být víc, tohle je první věc, co se rozbije.
static QueueHandle_t     s_queue = nullptr;
static volatile uint32_t s_drops = 0;

// ─── Počítání deautentizačních rámců ─────────────────────────────────────────
//
// Deauth NEJDE frontou, a to je podstatné rozhodnutí. Beacon nese data, která
// se musí přenést celá. Deauth potřebuje jen zvýšit počítadlo, což callback
// zvládne sám — a fronta na 16 záznamů by se při náraz stovek rámců za
// sekundu zaplnila okamžitě. Nástroj by tedy podhodnocoval tím víc, čím
// silnější náraz by měřil. To je vlastnost, kterou nechceme mít nikde.
struct DeauthSlot {
  volatile uint32_t generation;   // ++ při každém (pře)přidělení slotu
  volatile uint32_t deauth;       // monotónní, nikdy se nenuluje zvenčí
  volatile uint32_t disassoc;
  volatile uint32_t broadcast;
  volatile uint32_t lastFrameMs;
  volatile uint16_t lastReason;
  volatile uint8_t  channel;
  volatile bool     signedAsAp;
  uint8_t           bssid[6];     // píše se JEN pod zámkem
};
// sizeof(DeauthSlot) = 32 B, 12 slotů = 384 B.

static DeauthSlot       s_slots[DEAUTH_SLOTS];
static volatile uint8_t s_slotsUsed = 0;

// Celkové součty. Zvyšují se u KAŽDÉHO rámce, i když se slot nenajde nebo se
// musel recyklovat — díky tomu nemůže žádný rámec zmizet beze stopy.
static volatile uint32_t s_deauthTotal    = 0;
static volatile uint32_t s_disassocTotal  = 0;
static volatile uint32_t s_broadcastTotal = 0;
static volatile uint32_t s_recycled       = 0;   // kolikrát se slot přepsal

// Zámek chrání identitu slotu a nulování jeho počítadel při recyklaci — tedy
// vše, co se u slotu mění naráz a musí k sobě patřit.
//
// Šest bajtů MAC adresy se atomicky zapsat nedá, a kdyby loop() přečetlo
// adresu uprostřed přepisu, přiřadilo by počty špatné síti — a to už není
// nepřesnost, to je tvrzení. Zvažoval jsem místo zámku seqlock, ale ten by se
// opíral o pořadí zápisů mezi jádry, které volatile v C++ negarantuje;
// garantuje jen, že to nepřerovná kompilátor. Zámek je dokumentovaná záruka.
//
// Cena je nulová tam, kde na ní záleží: identita se zapisuje jen při přidělení
// slotu, tedy jednou za BSSID. V horké cestě, kdy prší stovky rámců za
// sekundu, se zvyšuje jen 32bitové počítadlo a zámek se nebere vůbec.
//
// portENTER_CRITICAL zakazuje přerušení na daném jádře, takže uvnitř je
// opravdu jen memcpy šesti bajtů a pár přiřazení. Žádný cyklus, žádné volání.
static portMUX_TYPE s_slotMux = portMUX_INITIALIZER_UNLOCKED;

// Počítadlo zahozených rámců: jeden zapisovatel (WiFi task), jeden čtenář
// (loop). Zarovnaný 32bitový zápis je na Xtensa atomický a víc zapisovatelů
// není, takže se hodnota nemůže ztratit ani roztrhnout napůl. Zámek by tady
// byl navíc — čtenář nanejvýš uvidí o pár rámců starší číslo, což u
// diagnostického počítadla nevadí.

// Zvýšení monotónního počítadla. Rozepsané na čtení a zápis, protože v++ nad
// volatile je od C++20 deprecated (-Wvolatile). Jediný zapisovatel je WiFi
// task, takže se nemůže nic ztratit; loop() jen čte.
static inline void bump(volatile uint32_t& counter) {
  const uint32_t v = counter;
  counter = v + 1;
}

// Volá se z callbacku. Musí být krátká a se shora omezeným počtem kroků.
static void countDeauth(const dot11::DeauthInfo& d, bool isDeauth, uint8_t channel) {
  // Celkové součty první — platí, ať se slot najde nebo ne.
  if (isDeauth) bump(s_deauthTotal);
  else          bump(s_disassocTotal);
  if (d.broadcast) bump(s_broadcastTotal);

  // Hledání slotu je bez zámku schválně: identity zapisuje jen tenhle
  // callback, tedy jediný task, a sám se se sebou nepotká. Zámek je tu kvůli
  // loop(), které čte.
  const uint8_t used = s_slotsUsed;
  int8_t idx = -1;
  for (uint8_t i = 0; i < used; i++) {
    if (memcmp(s_slots[i].bssid, d.bssid, sizeof(d.bssid)) == 0) {
      idx = (int8_t)i;
      break;
    }
  }

  if (idx < 0) {
    uint8_t target;
    if (used < DEAUTH_SLOTS) {
      target = used;
    } else {
      // Tabulka je plná → recykluje se nejdéle nečinný slot. Bez recyklace by
      // ji natrvalo zabraly jednorázové legitimní deauth a náraz by neměl kam.
      // Že se recyklovalo, se počítá a jde to na sériovku — nikdy to není tiché.
      target = 0;
      uint32_t oldest = s_slots[0].lastFrameMs;
      for (uint8_t i = 1; i < DEAUTH_SLOTS; i++) {
        // Rozdíl se znaménkem, aby to přežilo přetečení millis().
        if ((int32_t)(s_slots[i].lastFrameMs - oldest) < 0) {
          oldest = s_slots[i].lastFrameMs;
          target = i;
        }
      }
      bump(s_recycled);
    }

    // JEDINÉ místo v projektu, kde se sahá na identitu slotu.
    portENTER_CRITICAL(&s_slotMux);
    memcpy(s_slots[target].bssid, d.bssid, sizeof(d.bssid));
    s_slots[target].generation = s_slots[target].generation + 1;
    s_slots[target].deauth     = 0;
    s_slots[target].disassoc   = 0;
    s_slots[target].broadcast  = 0;
    if (target >= used) s_slotsUsed = (uint8_t)(target + 1);
    portEXIT_CRITICAL(&s_slotMux);

    idx = (int8_t)target;
  }

  DeauthSlot& s = s_slots[idx];
  if (isDeauth) bump(s.deauth);
  else          bump(s.disassoc);
  if (d.broadcast) bump(s.broadcast);

  s.lastReason  = d.reason;
  s.channel     = channel;
  s.signedAsAp  = d.signedAsAp;
  s.lastFrameMs = millis();
}

// ─── Callback promiskuitního režimu ──────────────────────────────────────────
//
// BĚŽÍ V KONTEXTU WiFi TASKU, ne v přerušení. Z toho plyne dvojí:
//
//   * volá se xQueueSend(), NE xQueueSendFromISR() — tohle se plete často;
//   * nemá tu co dělat IRAM_ATTR, to je pro obsluhy přerušení a jen by
//     ubralo z IRAM, které je vzácné.
//
// Co tady nesmí být za žádnou cenu: Serial, malloc/new, cokoliv blokujícího
// a hlavně JAKÝKOLI dotyk NetList. Ta patří výhradně loop(). Souběh se tady
// neřeší zámky — řeší se tím, že se nesdílí nic než fronta.
static void promiscRx(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;   // filtr to má odchytit, ale pojistka
  if (s_queue == nullptr)    return;

  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;

  if (pkt->rx_ctrl.rx_state != 0) return;   // rámec dorazil poškozený

  // sig_len je podle hlavičky core "length of packet including Frame Check
  // Sequence(FCS)". Kontrola minimální délky je tady i uvnitř parseBeacon —
  // schválně dvakrát, protože frameType() níž čte f[0] a musí mít jistotu.
  const uint16_t len = (uint16_t)pkt->rx_ctrl.sig_len;
  if (len < dot11::MIN_MGMT_FRAME) return;

  const uint8_t* f = pkt->payload;
  if (dot11::frameType(f) != dot11::TYPE_MGMT) return;

  const uint8_t sub = dot11::frameSubtype(f);

  // Deautentizace a disasociace. Filtr MGMT je propouští spolu s beacony,
  // takže se na něj nesahá. Tahle větev nesmí sáhnout na frontu — viz
  // komentář u tabulky slotů nahoře.
  if (sub == dot11::SUBTYPE_DEAUTH || sub == dot11::SUBTYPE_DISASSOC) {
    dot11::DeauthInfo d;
    if (!dot11::parseDeauth(f, len, d)) return;
    countDeauth(d, sub == dot11::SUBTYPE_DEAUTH, (uint8_t)pkt->rx_ctrl.channel);
    return;
  }

  if (sub != dot11::SUBTYPE_BEACON && sub != dot11::SUBTYPE_PROBE_RESP) return;

  NetInfo info;
  if (!dot11::parseBeacon(f, len, info)) return;

  // Co v rámci není a musí se doplnit z metadat rádia.
  info.rssi = (int8_t)pkt->rx_ctrl.rssi;
  if (info.channel == 0) {
    // Beacon neměl DS Parameter Set — spadneme na kanál, na kterém jsme
    // rámec slyšeli. Může být o jeden vedle kvůli překryvu kanálů.
    info.channel = (uint8_t)pkt->rx_ctrl.channel;
  }

  // Neblokující zápis. Když je fronta plná, rámec se zahodí a započítá;
  // nikdy se tu nečeká. Ten AP se ozve dalším beaconem za ~100 ms.
  if (xQueueSend(s_queue, &info, 0) != pdTRUE) {
    // Rozepsané na čtení a zápis schválně: s_drops++ nad volatile je od
    // C++20 deprecated (-Wvolatile), protože u složeného přiřazení není
    // jasně dané pořadí přístupů. Takhle je vidět, že jde o jedno čtení
    // a jeden zápis — a protože je zapisovatel jediný, nic se neztratí.
    const uint32_t d = s_drops;
    s_drops = d + 1;
  }
}

// ─── BssidRoster ─────────────────────────────────────────────────────────────

void BssidRoster::clear() {
  count_      = 0;
  overflowed_ = false;
}

void BssidRoster::touch(const uint8_t* bssid, uint32_t nowMs) {
  // Lineární hledání přes max. 96 položek. Běží v loop(), ne v callbacku,
  // a i při stovkách beaconů za sekundu je to pár desítek tisíc porovnání
  // šesti bajtů — na 240 MHz nic. Hashovací tabulka by tu byla složitost
  // bez užitku.
  for (uint16_t i = 0; i < count_; i++) {
    if (memcmp(items_[i].bssid, bssid, sizeof(items_[i].bssid)) == 0) {
      items_[i].lastSeenMs = nowMs;
      return;
    }
  }

  if (count_ >= PROMISC_ROSTER_CAP) {
    // Síť, kterou jsme ještě neznali, a není pro ni místo ani tady. Od téhle
    // chvíle nevíme, kolik sítí kolem nás vlastně je — a přesně to musí být
    // vidět na displeji, ne jen v README.
    overflowed_ = true;
    return;
  }

  memcpy(items_[count_].bssid, bssid, sizeof(items_[count_].bssid));
  items_[count_].lastSeenMs = nowMs;
  count_++;
}

void BssidRoster::expireOlderThan(uint32_t nowMs, uint32_t maxAgeMs) {
  uint16_t i = 0;
  while (i < count_) {
    if (nowMs - items_[i].lastSeenMs >= maxAgeMs) {
      for (uint16_t k = i; k + 1 < count_; k++) items_[k] = items_[k + 1];
      count_--;               // i se nezvyšuje, na jeho místo se posunul další
    } else {
      i++;
    }
  }
}

// ─── PromiscSource ───────────────────────────────────────────────────────────

void PromiscSource::begin() {
  nets_.clear();
  roster_.clear();
  s_drops    = 0;
  lastDrops_ = 0;

  if (s_queue == nullptr) {
    s_queue = xQueueCreate(PROMISC_QUEUE_LEN, sizeof(NetInfo));
  }

  // Rádio se nastartuje jako stanice, ale nikam se nepřipojuje a nic
  // nevysílá — scanNetworks() se tady nevolá právě proto, že je aktivní
  // a rozesílal by probe requesty.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);   // ať rádio neusíná a neztrácíme rámce

  // Filtr na management rámce. Bez něj by callback dostával i všechna data,
  // což je násobně víc práce ve WiFi tasku úplně zbytečně.
  wifi_promiscuous_filter_t filter;
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filter);

  esp_wifi_set_promiscuous_rx_cb(&promiscRx);
  esp_wifi_set_promiscuous(true);

  channel_ = PROMISC_CHAN_FIRST;
  retune(channel_);

  const uint32_t now = millis();
  dwellStartedAt_ = now;
  sweepStartedAt_ = now;

  updateStatus();
  diag_[0] = '\0';
}

void PromiscSource::retune(uint8_t ch) {
  // Přelaďuje se výhradně odsud, tedy z loop(). Volat to z callbacku, tedy
  // z WiFi tasku uprostřed zpracování rámce, si koleduje o problém.
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

bool PromiscSource::poll() {
  const uint32_t now = millis();

  // ── 1) Vysát frontu ──
  // Děje se při KAŽDÉM průchodu, ať se nic nehromadí. Tohle je jediné místo
  // v celém projektu, kde data z callbacku vstupují do NetList.
  NetInfo info;
  while (s_queue != nullptr && xQueueReceive(s_queue, &info, 0) == pdTRUE) {
    // Razítko se dává až tady schválně: callback má být co nejkratší a
    // zpoždění mezi příjmem a vybráním z fronty je řádově mikrosekundy.
    info.lastSeenMs = now;
    roster_.touch(info.bssid, now);
    nets_.upsertByBssid(info);
  }

  // ── 2) Přeladit, když vypršela prodleva ──
  bool sweepDone = false;
  if (now - dwellStartedAt_ >= PROMISC_DWELL_MS) {
    dwellStartedAt_ = now;
    if (channel_ >= PROMISC_CHAN_LAST) {
      channel_  = PROMISC_CHAN_FIRST;
      sweepDone = true;                 // právě jsme dokončili celou otočku
    } else {
      channel_++;
    }
    retune(channel_);
    updateStatus();
  }

  if (!sweepDone) return false;

  // ── 3) Konec otočky = obdoba dokončeného skenu ──
  // Seznam se "vydává" jen tady, ne při každém beaconu. Jinak by sériový
  // výpis běžel několikrát za sekundu a .ino by se muselo měnit.
  lastCollectMs_  = now - sweepStartedAt_;
  sweepStartedAt_ = now;

  roster_.expireOlderThan(now, PROMISC_EXPIRE_MS);
  nets_.expireOlderThan(now, PROMISC_EXPIRE_MS);

  nets_.setSeen(roster_.count(), roster_.overflowed());

  // Vyhodnocení nárazu musí být před updateStatus(), aby se "!" v hlavičce
  // objevilo hned v téhle otočce a ne až v další.
  evaluateDeauth(now);
  updateStatus();
  updateDiagnostics();

  // Příznak přetečení platí vždy pro okno mezi dvěma otočkami. Kdyby byl
  // trvalý, zůstalo by "+" na displeji navěky i po odchodu z husté zástavby.
  roster_.clearOverflow();

  return true;
}

void PromiscSource::evaluateDeauth(uint32_t now) {
  // KONZISTENTNÍ SNÍMEK CELÉHO SLOTU POD ZÁMKEM.
  //
  // Dřív se pod zámkem braly jen adresy a počítadla se četla až mimo něj.
  // To mělo závodní podmínku: kdyby callback mezi čtením generace a čtením
  // počítadel slot recykloval, prošla by kontrola generace ještě se starou
  // hodnotou, ale počítadla by se přečetla už vynulovaná. Rozdíl 0 - prev_
  // by podtekl na obrovské číslo a vyhodnotil se jako gigantický náraz.
  //
  // Recyklace nuluje počítadla uvnitř kritické sekce (viz countDeauth), takže
  // když se generace i počty přečtou taky pod zámkem, patří k sobě. Prosté
  // inkrementy z horké cesty zámek neberou, ale ty jsou monotónní — můžou
  // hodnotu jen zvýšit, nikdy nerozhodit dvojici generace/počet.
  //
  // Cena je 12 × 32 bajtů kopírování navíc: samá přiřazení a memcpy šesti
  // bajtů, žádný cyklus s voláním uvnitř.
  struct Snap {
    uint8_t  bssid[6];
    uint32_t generation;
    uint32_t deauth;
    uint32_t disassoc;
    uint32_t broadcast;
    uint16_t lastReason;
    uint8_t  channel;
    bool     signedAsAp;
  };
  Snap    snap[DEAUTH_SLOTS];
  uint8_t used;

  portENTER_CRITICAL(&s_slotMux);
  used = s_slotsUsed;
  for (uint8_t i = 0; i < used; i++) {
    memcpy(snap[i].bssid, s_slots[i].bssid, sizeof(snap[i].bssid));
    snap[i].generation = s_slots[i].generation;
    snap[i].deauth     = s_slots[i].deauth;
    snap[i].disassoc   = s_slots[i].disassoc;
    snap[i].broadcast  = s_slots[i].broadcast;
    snap[i].lastReason = s_slots[i].lastReason;
    snap[i].channel    = s_slots[i].channel;
    snap[i].signedAsAp = s_slots[i].signedAsAp;
  }
  portEXIT_CRITICAL(&s_slotMux);

  // Příznaky se každou otočku zhasnou a rozsvítí znovu podle toho, co je
  // ještě v době držení. Idempotentní, takže nemůže nikde zůstat viset.
  nets_.clearDeauthFlags();
  burstOnRecord_ = false;
  newBurst_      = -1;
  burstCount_    = 0;

  for (uint8_t i = 0; i < used; i++) {
    // Pojistka nad rámec snímku: monotónní počítadlo klesnout nemůže, takže
    // nižší hodnota než minule je jednoznačná známka, že se slot pod rukama
    // přepsal. Rozdíl se pak nebere, aby z podtečení nevyšel falešný náraz.
    const bool wentBackwards = (snap[i].deauth    < prev_[i].deauth)
                            || (snap[i].disassoc  < prev_[i].disassoc)
                            || (snap[i].broadcast < prev_[i].broadcast);

    if (snap[i].generation != prev_[i].generation || wentBackwards) {
      // Slot mezitím dostal jiné BSSID. Předchozí hodnoty patřily někomu
      // jinému, takže se zahodí — jinak by z nich vyšel nesmyslný rozdíl.
      prev_[i].generation = snap[i].generation;
      prev_[i].deauth     = snap[i].deauth;
      prev_[i].disassoc   = snap[i].disassoc;
      prev_[i].broadcast  = snap[i].broadcast;
      prev_[i].burstAtMs  = 0;
      continue;   // v téhle otočce se z tohohle slotu nic nevyhodnocuje
    }

    const uint32_t dDeauth = snap[i].deauth    - prev_[i].deauth;
    const uint32_t dDisas  = snap[i].disassoc  - prev_[i].disassoc;
    const uint32_t dBcast  = snap[i].broadcast - prev_[i].broadcast;

    prev_[i].deauth    = snap[i].deauth;
    prev_[i].disassoc  = snap[i].disassoc;
    prev_[i].broadcast = snap[i].broadcast;

    // Práh se vztahuje k jedné otočce, tedy k jednomu 250ms oknu na tom
    // kanálu. Odvození obou čísel je v config.h.
    if ((dDeauth + dDisas) >= DEAUTH_BURST_MIN || dBcast >= DEAUTH_BCAST_MIN) {
      prev_[i].burstAtMs = (now != 0) ? now : 1;   // 0 znamená "nic"
      burstCount_++;

      // Podrobnosti se nesou jen o jednom nárazu. Kolik jich v téhle otočce
      // bylo celkem, hlásí burstCount_ — displej označí všechny dotčené sítě,
      // takže výpis nesmí tvrdit, že byla jen jedna.
      newBurst_        = (int8_t)i;
      burstFrames_     = dDeauth + dDisas;
      burstBcast_      = dBcast;
      burstReason_     = snap[i].lastReason;
      burstChannel_    = snap[i].channel;
      burstSignedAsAp_ = snap[i].signedAsAp;
      memcpy(burstBssid_, snap[i].bssid, sizeof(burstBssid_));
    }

    if (prev_[i].burstAtMs != 0 && (now - prev_[i].burstAtMs) < DEAUTH_HOLD_MS) {
      burstOnRecord_ = true;
      // Vrátí false, když už síť vypadla stárnutím ze seznamu. To je v pořádku
      // a je to vědomé: posouvat kvůli deauth rámci lastSeenMs by znamenalo
      // tvrdit, že jsme slyšeli beacon, který jsme neslyšeli.
      nets_.setDeauthFlag(snap[i].bssid);
    } else {
      prev_[i].burstAtMs = 0;
    }
  }
}

void PromiscSource::updateStatus() {
  // "P6" = promiskuitní režim, kanál 6. "!6" = totéž a na kontě je náraz.
  //
  // Vyměněné písmeno místo přidaného znaku schválně: hlavička je v nejhorším
  // případě 2 px od čísla stránky, takže na nic navíc není místo. A to "P"
  // je stejně redundantní — že jsi v promisk režimu, poznáš podle toho, že
  // je tam kanál.
  snprintf(status_, sizeof(status_), "%c%u",
           burstOnRecord_ ? '!' : 'P', (unsigned)channel_);
}

// Najde secFlags sítě podle BSSID. Vrací 0xFF, když ji v seznamu nemáme —
// pak o ochraně management rámců prostě nic nevíme a netvrdíme nic.
static uint8_t secFlagsOf(const NetList& nets, const uint8_t* bssid) {
  for (uint8_t i = 0; i < nets.count(); i++) {
    if (memcmp(nets.at(i).bssid, bssid, 6) == 0) return nets.at(i).secFlags;
  }
  return 0xFF;
}

void PromiscSource::updateDiagnostics() {
  const uint32_t drops       = s_drops;      // jednotlivá atomická čtení
  const uint32_t recycled    = s_recycled;
  const uint32_t dropsDelta  = drops - lastDrops_;
  const uint32_t recycDelta  = recycled - lastRecycled_;
  lastDrops_    = drops;
  lastRecycled_ = recycled;

  size_t n = 0;
  diag_[0] = '\0';

  // snprintf vrací délku, kterou by ZAPSAL, ne kterou zapsal. Kdyby se to
  // sčítalo naivně, n by při useknutí přerostlo buffer a další diag_ + n by
  // ukazovalo za konec pole. Tohle to po každém kroku přiskřípne.
  const auto put = [&](int written) {
    if (written <= 0) return;
    n += (size_t)written;
    if (n >= sizeof(diag_)) n = sizeof(diag_) - 1;
  };
  const auto newline = [&]() {
    if (n > 0 && n + 1 < sizeof(diag_)) {
      diag_[n++] = '\n';
      diag_[n]   = '\0';
    }
  };

  if (newBurst_ >= 0) {
    put(snprintf(diag_ + n, sizeof(diag_) - n,
      "deauth naraz: %lu+ ramcu podepsanych jako %02X:%02X:%02X:%02X:%02X:%02X"
      ", kanal %u\n",
      (unsigned long)burstFrames_,
      burstBssid_[0], burstBssid_[1], burstBssid_[2],
      burstBssid_[3], burstBssid_[4], burstBssid_[5],
      (unsigned)burstChannel_));

    put(snprintf(diag_ + n, sizeof(diag_) - n,
      "      z toho broadcast %lu, posledni reason %u (%s), podepsano jako %s\n",
      (unsigned long)burstBcast_,
      (unsigned)burstReason_,
      dot11::reasonName(burstReason_),
      burstSignedAsAp_ ? "samo AP" : "jina adresa nez BSSID"));

    // Kontext, který úplně mění, co ten náraz znamená.
    const uint8_t sf = secFlagsOf(nets_, burstBssid_);
    if (sf == 0xFF) {
      put(snprintf(diag_ + n, sizeof(diag_) - n,
        "      sit nemame v seznamu, o ochrane management ramcu nevime nic\n"));
    } else if (sf & SEC_MFPR) {
      put(snprintf(diag_ + n, sizeof(diag_) - n,
        "      sit VYZADUJE ochranu management ramcu (802.11w): podvrzene\n"
        "      deauth ramce na ni nemaji ucinek. Presto je vidime a pocitame.\n"));
    } else if (sf & SEC_MFPC) {
      put(snprintf(diag_ + n, sizeof(diag_) - n,
        "      sit ochranu management ramcu umi, ale nevyzaduje ji\n"));
    } else {
      put(snprintf(diag_ + n, sizeof(diag_) - n,
        "      sit ochranu management ramcu nenabizi\n"));
    }

    put(snprintf(diag_ + n, sizeof(diag_) - n,
      "      DOLNI ODHAD: kazdy kanal poslouchame ~250 z 3250 ms (~8 %% casu).\n"
      "      Neextrapolujeme - kolikrat vic proletelo, nevime.\n"
      "      MAC se u deauth podvrhuje. Vime, JAKYM JMENEM je ramec podepsan,\n"
      "      ne kdo ho poslal. Znacka u site znamena DOTCENA, ne odpovedna."));

    // V jedné otočce může mít náraz víc sítí. Displej označí všechny, takže
    // výpis nesmí mlčet o tom, že podrobnosti výš patří jen jedné z nich.
    if (burstCount_ > 1) {
      put(snprintf(diag_ + n, sizeof(diag_) - n,
        "\n      naraz melo v teto otocce jeste dalsich %u siti - podrobnosti"
        " vyse patri jen jedne z nich, oznaceny jsou vsechny",
        (unsigned)(burstCount_ - 1)));
    }
  }

  if (dropsDelta > 0) {
    newline();
    put(snprintf(diag_ + n, sizeof(diag_) - n,
      "      zahozeno beaconu: %lu za otocku, %lu celkem (fronta byla plna)",
      (unsigned long)dropsDelta, (unsigned long)drops));
  }

  if (recycDelta > 0) {
    newline();
    put(snprintf(diag_ + n, sizeof(diag_) - n,
      "      recyklovano deauth slotu: %lu za otocku (sledujeme vic BSSID nez"
      " DEAUTH_SLOTS=%u, starsi pocty se ztratily)",
      (unsigned long)recycDelta, (unsigned)DEAUTH_SLOTS));
  }

  if (roster_.overflowed()) {
    newline();
    put(snprintf(diag_ + n, sizeof(diag_) - n,
      "      roster plny, seen() je dolni odhad"));
  }
}

#endif  // NET_SOURCE == NET_SOURCE_PROMISC
