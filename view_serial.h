#pragma once
#include <stdint.h>
#include "netlist.h"

// ─── VÝSTUP NA SÉRIOVOU LINKU ────────────────────────────────────────────────
//
// Druhý výstup nad stejným modelem. Displej je pro přehled, sériovka pro
// ladění — proto se tady SSID nezkracuje a přidává se navíc BSSID.

void serialBegin();
void serialDump(const NetList& nets, uint32_t collectMs);
