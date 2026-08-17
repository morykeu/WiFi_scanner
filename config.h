#pragma once
#include <stdint.h>

// ─── Volba zdroje dat ────────────────────────────────────────────────────────
//
// Přepínač je překladový, ne běhový: promiskuitní režim a scanNetworks() se
// v rádiu vylučují, takže mezi nimi stejně nejde přepínat za běhu bez
// restartu WiFi stacku. A tlačítko na desce zatím není.
//
// Změň NET_SOURCE a přelož znovu.
#define NET_SOURCE_SCAN     0
#define NET_SOURCE_PROMISC  1

#define NET_SOURCE          NET_SOURCE_PROMISC

// ─── Kapacity ────────────────────────────────────────────────────────────────
//
// Kolik sítí si nejvíc pamatujeme. Paměť to nerozhoduje: sizeof(NetInfo) je
// 48 B (viz netlist.h), takže 24 × 48 = 1152 B v .bss — proti ~300 kB volné
// DRAM po nastartování WiFi stacku nezajímavé. Limitem je čitelnost:
// 24 sítí = 5 stránek × PAGE_MS = 15 s na jeden celý průchod displejem.
static const uint8_t MAX_NETS = 24;

// ─── Časování společné oběma zdrojům ─────────────────────────────────────────
static const uint32_t PAGE_MS     = 3000;
static const uint32_t SERIAL_BAUD = 115200;

// ─── Časování skenu ──────────────────────────────────────────────────────────

// Pauza mezi skeny, měřená od DOKONČENÍ předchozího skenu (ne od jeho
// začátku). Kdyby se měřila od začátku, kolísavá doba skenu (2–4 s) by se
// vlévala do periody a stránkování by se rozjelo proti skenům.
static const uint32_t SCAN_PERIOD_MS = 15000;

// Pojistka: kdyby se asynchronní sken nikdy neohlásil jako hotový, po téhle
// době ho odpískáme a zkusíme znovu. Sken sám trvá 2–4 s, tohle je 3× tolik.
static const uint32_t SCAN_TIMEOUT_MS = 12000;

// ─── Časování promiskuitního režimu ──────────────────────────────────────────

// Kanály 1–13, tedy ETSI / Česko. Kanál 14 je jen Japonsko a 12–13 nesmí
// v USA, ale poslouchat se smí všude — vysíláme nula rámců.
static const uint8_t PROMISC_CHAN_FIRST = 1;
static const uint8_t PROMISC_CHAN_LAST  = 13;

// Jak dlouho posloucháme na jednom kanálu, než přeladíme.
//
// ODVOZENÍ. Beacon interval je typicky 100 TU = 102,4 ms. V okně délky L je
// zaručeně aspoň floor(L / 102,4) beaconů od každé slyšitelné sítě, a to bez
// ohledu na to, jak se do jejich vysílání fázově trefíme:
//
//     dwell    zaručených beaconů    celá otočka 13 kanálů
//     150 ms          1                    1,95 s
//     200 ms          1                    2,60 s
//     250 ms          2                    3,25 s   ← tady
//     300 ms          2                    3,90 s
//
// Jedna příležitost je málo: beacony se zpožďují kolizemi na kanálu a slabý
// AP se občas neslyší vůbec. Dvě jsou rozumné minimum. 250 ms je nejkratší
// doba, která dvě zaručuje — 300 ms nedá víc jistoty, jen delší otočku.
//
// Vedlejší efekt: otočka 3,25 s vyjde skoro přesně na dobu jednoho
// scanNetworks(), takže se tempo displeje mezi oběma režimy nemění.
static const uint32_t PROMISC_DWELL_MS = 250;

// Po jak dlouhém tichu záznam ze seznamu vypadne.
//
// ODVOZENÍ. Jedna otočka trvá 3,25 s, takže 30 s je asi 9 otoček. Kratší doba
// by blikala — že se slabý AP v jedné otočce neslyší, je běžné, ne výjimečné.
// Delší by nechávala odstěhované sítě viset na displeji dlouho po tom, co
// zmizely. Sken tenhle problém nemá, ten seznam pokaždé staví znovu.
static const uint32_t PROMISC_EXPIRE_MS = 30000;

// Hloubka fronty mezi callbackem WiFi tasku a loop().
//
// loop() frontu vysává celou při každém průchodu a běží tisíckrát za sekundu,
// takže fronta musí pobrat jen dávku, která přijde mezi dvěma průchody.
// 16 × 48 B = 768 B je řádově víc, než je potřeba. Jestli jsem se spletl,
// řekne to počítadlo zahozených rámců na sériové lince.
static const uint8_t PROMISC_QUEUE_LEN = 16;

// ─── Detekce deautentizačních rámců ──────────────────────────────────────────
//
// Pozorovací kvantum je dané přelaďováním: každý kanál slyšíme 250 ms
// z 3250 ms, tedy asi 7,7 % času. Prahy níž se vztahují k JEDNOMU takovému
// oknu, ne k sekundě.

// Kolik BSSID se počítá zvlášť. Při nárazu bývá cílená jedna dvě sítě; zbytek
// je zásoba na legitimní deauth z okolních AP po dobu držení příznaku.
// 12 × 32 B = 384 B. Když dojdou, rámce se pořád počítají do celkového součtu
// a do "nepřiřazeno" — nikdy se netratí tiše.
static const uint8_t DEAUTH_SLOTS = 12;

// Práh nárazu: kolik deauth + disassoc rámců od jednoho BSSID v jednom okně.
//
// ODVOZENÍ. Legitimní deauth je jednotlivý jev: klient odchází → 1 rámec,
// s opakováním 2–3. AP odkopne nečinného klienta → 1. V okně 250 ms tedy
// z jednoho BSSID běžně 0, výjimečně 1–3.
//
// Nástroje na zaplavení posílají desítky až stovky rámců za sekundu. Práh 5:
//   * má rezervu nad legitimním opakováním, které končí u tří,
//   * a přitom stačí i na vlažný náraz od ~20 rámců/s (5 ÷ 0,25 s).
static const uint16_t DEAUTH_BURST_MIN = 5;

// Zvláštní, nižší práh pro broadcast (adresa 1 = ff:ff:ff:ff:ff:ff).
//
// ODVOZENÍ. Broadcast deauth odpojuje všechny klienty naráz. Legitimně přijde
// jeden, když se AP restartuje nebo mění konfiguraci. Dva ve čtvrt sekundě
// už restart není. Silnější signál si zaslouží nižší práh.
static const uint16_t DEAUTH_BCAST_MIN = 2;

// Jak dlouho se příznak drží po nárazu.
//
// ODVOZENÍ. Náraz je za pár sekund pryč. Kdyby příznak zmizel s ním, uvidíš
// ho jen když se zrovna koukáš. Minuta stačí, aby sis toho všiml, a je dost
// krátká, aby se poznalo "děje se to teď" od "stalo se to".
//
// Vědomý důsledek: síť může vypadnout z NetList stárnutím (30 s) dřív, než
// příznak vyprší (60 s). Neřešíme to posouváním lastSeenMs — to by znamenalo
// tvrdit, že jsme slyšeli beacon, který jsme neslyšeli. Náraz pak zůstane
// v hlavičce a na sériovce, jen nebude co zvýraznit.
static const uint32_t DEAUTH_HOLD_MS = 60000;

// Kapacita rosteru BSSID — seznamu "koho jsme slyšeli", nezávislého na tom,
// kolik sítí se vejde do NetList. Díky němu umí hlavička poctivě hlásit
// "24/37" i v promiskuitním režimu. 96 × 12 B = 1152 B.
//
// Když se naplní i tenhle, hlásí se to na displeji jako "24/96+" — tedy
// "aspoň 96". Pravidlo je pořád stejné: když nástroj neví, kolik toho neví,
// musí to říct.
//
// POZOR při zvyšování: nad 99 má číslo tři číslice a hlavička se v nejhorším
// případě přestane vejít vedle čísla stránky. Viz výpočet ve view_oled.cpp.
static const uint8_t PROMISC_ROSTER_CAP = 96;
