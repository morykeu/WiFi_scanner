#include <Arduino.h>
#include <stdio.h>
#include "view_serial.h"
#include "config.h"

// Pro sériovku má zabezpečení jméno, ne jeden znak — místa je tady dost.
static const char* authName(AuthKind a) {
  switch (a) {
    case AUTH_OPEN:       return "open";
    case AUTH_WEP:        return "WEP";
    case AUTH_WPA:        return "WPA";
    case AUTH_WPA2:       return "WPA2";
    case AUTH_WPA3:       return "WPA3";
    case AUTH_ENTERPRISE: return "ENT";
    default:              return "?";
  }
}

void serialBegin() {
  Serial.begin(SERIAL_BAUD);
  // Jediné delay() v celém projektu, a to schválně: USB-serial na ESP32 chvíli
  // trvá, než se probere, a bez tohohle ti první výpis zmizí. V setup() to
  // nikomu nevadí, v loop() by to byl hřích.
  delay(200);
  Serial.println();
  Serial.println(F("=== ESP32 WiFi scanner ==="));
}

void serialDump(const NetList& nets, uint32_t collectMs) {
  static uint32_t scanNo = 0;
  scanNo++;

  // Když se něco nevešlo, musí to být vidět i tady, ne jen na displeji.
  // Jinak by ladicí výpis vypadal jako úplný seznam a nebyl by.
  if (nets.truncated()) {
    Serial.printf("\n--- sken #%lu: %u z %u siti, %u zahozeno (MAX_NETS=%u), %lu ms ---\n",
                  (unsigned long)scanNo,
                  (unsigned)nets.count(),
                  (unsigned)nets.seen(),
                  (unsigned)(nets.seen() - nets.count()),
                  (unsigned)MAX_NETS,
                  (unsigned long)collectMs);
  } else {
    Serial.printf("\n--- sken #%lu: %u siti, %lu ms ---\n",
                  (unsigned long)scanNo,
                  (unsigned)nets.count(),
                  (unsigned long)collectMs);
  }
  Serial.println(F(" #  ch  dBm  auth   bssid              ssid"));

  // Rozpočet na řádek: 2+2 + 2+1 + 4+2 + 5+2 + 17+2 + 32 = 71 znaků + NUL.
  // 110 je s rezervou a leží to na stacku loop tasku, který má 8 kB.
  char line[110];

  for (uint8_t i = 0; i < nets.count(); i++) {
    const NetInfo& n = nets.at(i);
    const char* ssid = n.ssid[0] ? n.ssid : "<hidden>";

    snprintf(line, sizeof(line),
             "%2u  %2u %4d  %-5s  %02X:%02X:%02X:%02X:%02X:%02X  %s",
             (unsigned)(i + 1),
             (unsigned)n.channel,
             (int)n.rssi,
             authName(n.auth),
             n.bssid[0], n.bssid[1], n.bssid[2],
             n.bssid[3], n.bssid[4], n.bssid[5],
             ssid);   // celé, nezkrácené — na tom je ta sériovka dobrá

    Serial.println(line);
  }
}
