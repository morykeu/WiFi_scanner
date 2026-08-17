#pragma once
#include <stdint.h>

// ─── Kapacity ────────────────────────────────────────────────────────────────
//
// Kolik sítí si nejvíc pamatujeme. Paměť to nerozhoduje: sizeof(NetInfo) je
// 42 B (viz netlist.h), takže 24 × 42 = 1008 B v .bss — proti ~300 kB volné
// DRAM po nastartování WiFi stacku nezajímavé. Limitem je čitelnost:
// 24 sítí = 5 stránek × PAGE_MS = 15 s na jeden celý průchod displejem.
// Když to zvedneš, zkrať i PAGE_MS, jinak budou data na poslední stránce
// starší než další sken.
static const uint8_t MAX_NETS = 24;

// ─── Časování ────────────────────────────────────────────────────────────────

// Pauza mezi skeny, měřená od DOKONČENÍ předchozího skenu (ne od jeho
// začátku). Kdyby se měřila od začátku, kolísavá doba skenu (2–4 s) by se
// vlévala do periody a stránkování by se rozjelo proti skenům.
static const uint32_t SCAN_PERIOD_MS = 15000;

// Jak dlouho svítí jedna stránka. 5 stránek × 3 s = 15 s ≈ jeden scan cyklus.
static const uint32_t PAGE_MS = 3000;

// Pojistka: kdyby se asynchronní sken nikdy neohlásil jako hotový, po téhle
// době ho odpískáme a zkusíme znovu. Sken sám trvá 2–4 s, tohle je 3× tolik.
static const uint32_t SCAN_TIMEOUT_MS = 12000;

static const uint32_t SERIAL_BAUD = 115200;
