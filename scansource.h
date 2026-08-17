#pragma once
#include <stdint.h>
#include "netsource.h"

#if NET_SOURCE == NET_SOURCE_SCAN

// Zdroj dat postavený na WiFi.scanNetworks() v asynchronním režimu.
//
// Celý stavový automat je schovaný tady. .ino jen volá poll() a neví, že
// nějaký automat existuje.
//
// Pozor: tenhle zdroj VYSÍLÁ. scanNetworks() je aktivní sken, tedy rozesílá
// probe requesty a čeká na odpovědi. Je to úplně běžný provoz stanice, ale je
// dobré to vědět — promiskuitní režim je proti tomu čistě pasivní a nevyšle nic.
class ScanSource : public NetSource {
public:
  void begin() override;
  bool poll() override;

  const NetList& nets() const override { return nets_; }
  uint32_t lastCollectMs() const override { return lastCollectMs_; }

  // Literály mají statickou dobu života, takže ukazatel volání přežije.
  const char* status() const override { return (state_ == RUNNING) ? "*" : ""; }
  const char* diagnostics() const override { return ""; }

private:
  enum State : uint8_t {
    IDLE,     // čekáme, až vyprší SCAN_PERIOD_MS
    RUNNING   // sken běží ve WiFi tasku, my se jen ptáme, jestli už je hotov
  };

  NetList  nets_;
  State    state_         = IDLE;
  uint32_t stateSince_    = 0;  // millis() poslední změny stavu
  uint32_t scanStartedAt_ = 0;
  uint32_t lastCollectMs_ = 0;

  void startScan();
  void collect(uint16_t found, bool capped);
};

#endif  // NET_SOURCE == NET_SOURCE_SCAN
