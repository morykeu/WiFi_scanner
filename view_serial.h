#pragma once
#include <stdint.h>
#include "netlist.h"

// ─── VÝSTUP NA SÉRIOVOU LINKU ────────────────────────────────────────────────
//
// Druhý výstup nad stejným modelem. Displej je pro přehled, sériovka pro
// ladění — proto se tady SSID nezkracuje a přidává se navíc BSSID.

void serialBegin();

// note je diagnostická poznámka od zdroje dat, nebo "" / nullptr když není co
// hlásit — zahozené rámce, přeplněný roster a podobně. Platí pro ni to samé,
// co pro status() v netsource.h: view neřeší, co v ní je, jen ji vytiskne.
void serialDump(const NetList& nets, uint32_t collectMs, const char* note);
