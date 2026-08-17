# ESP32 WiFi Scanner

A standalone 2.4 GHz WiFi survey tool for the ESP32 with a 128×64 OLED readout.
It works two ways, selected at compile time: it can run repeated **asynchronous
scans**, or it can sit in **promiscuous mode** and build the same list purely by
listening to beacons — which additionally lets it count deauthentication and
disassociation frames. Either way the display shows SSID, channel, signal
strength in dBm and a two-character encryption marker, sorted strongest-first,
paginated because five rows do not fit two dozen networks. The same data goes
out over the serial port in a wider format for debugging.

**This tool only ever listens.** It never transmits a frame in promiscuous
mode — no deauth, no probe requests, no injection, not even commented out.

![The scanner running: five networks on one page, sorted strongest-first](docs/display.jpg)

*Network names are redacted. The fourth row reads `12` — a WPA/WPA2 mixed-mode
network that earlier versions of this code would have reported as `2`. See
[Reporting the weakest accepted method](#reporting-the-weakest-accepted-method).*

<!-- Second photo slot: wiring. Drop the image in docs/ and uncomment:
     ![Wiring between the ESP32 board and the OLED](docs/wiring.jpg)
-->

What the display looks like, here in promiscuous mode:

```
 y=8    WiFi 24/37 AP P6       2/5
 y=10   ────────────────────────────
 y=21  12 Nazev-sit~      6  -41
 y=31   - FreeWifi        1  -58
 y=41   3 <hidden>       11  -67
 y=51  23 UPC1234567      6  -72     ← inverted when named in a deauth burst
 y=61   w StaraSitVe~     3 -100
```

| Element | Meaning |
|---|---|
| `12` (leading two chars) | encryption, by the **weakest method the network accepts** — see [below](#reporting-the-weakest-accepted-method) |
| `24/37` | 24 networks stored out of 37 seen — see [Showing what was dropped](#showing-what-was-dropped) |
| `24/96+` | the `+` means *at least* 96; the counter hit its own ceiling |
| `*` | a scan is running (scan mode only) |
| `P6` | promiscuous mode, currently listening on channel 6 |
| `!6` | same, and a **deauth burst is on record** — see [Counting deauth frames](#counting-deauthentication-frames) |
| inverted row | this network was **named in** a deauth burst |

## Hardware

- **ESP32-WROOM-32D** on a generic dev board (Arduino board type: *ESP32 Dev Module*)
- **128×64 OLED, SH1106 controller, 4-wire SPI**

| OLED pin | ESP32 pin | Note |
|---|---|---|
| GND | GND | |
| VCC | 3.3V | not 5V |
| SCK | 18 | hardware SPI clock, fixed by the SPI peripheral |
| SDA | 23 | hardware SPI MOSI, fixed by the SPI peripheral |
| RES | 17 | passed to the U8g2 constructor |
| DC  | 16 | passed to the U8g2 constructor |
| CS  | 5  | passed to the U8g2 constructor |

SCK and SDA are not free choices — they are the ESP32's hardware SPI pins, and
the `_4W_HW_SPI` U8g2 constructor uses them implicitly. Only RES, DC and CS are
arguments, and they appear in one place, at the top of
[`view_oled.cpp`](view_oled.cpp).

Some modules of this size ship with an SSD1306 controller instead. If the image
comes out shifted or scrambled, the alternative constructor is on the next line
of that file, commented out.

## Getting started

1. Install the [Arduino IDE](https://www.arduino.cc/en/software) (2.x).
2. **File → Preferences → Additional boards manager URLs**, add:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
3. **Tools → Board → Boards Manager**, search `esp32`, install
   *esp32 by Espressif Systems*. **This project targets the 3.x series** and was
   developed against **3.3.11**. It maps `WIFI_AUTH_OWE` and
   `WIFI_AUTH_WPA3_ENT_192`, which do not exist in core 2.x, so it will not
   compile there. There are no `#if` version guards: a stated target version is
   easier to trust than conditional compilation guessing at a threshold.
4. **Tools → Board → ESP32 Arduino → ESP32 Dev Module**.
5. **Tools → Manage Libraries**, search `U8g2`, install *U8g2 by oliver*.
6. Open [`WiFi_scanner.ino`](WiFi_scanner.ino). The other files appear as tabs
   automatically — Arduino compiles every `.ino`, `.cpp` and `.h` in the sketch
   folder, so there is nothing else to install or configure.
7. Select the serial port and upload. Open the Serial Monitor at **115200 baud**.

`WiFi.h` and `SPI.h` come with the ESP32 core. U8g2 is the only external
dependency.

### Choosing the data source

One line in [`config.h`](config.h):

```c
#define NET_SOURCE  NET_SOURCE_PROMISC   // or NET_SOURCE_SCAN
```

The switch is a compile-time one because the radio cannot do both at once:
promiscuous capture and `WiFi.scanNetworks()` are mutually exclusive. Arduino
compiles every `.cpp` in the sketch folder regardless, so the body of each
source file is wrapped in the same `#if` — the unused one is not built into the
image at all.

| | `NET_SOURCE_SCAN` | `NET_SOURCE_PROMISC` |
|---|---|---|
| How it finds networks | active scan: sends probe requests, collects answers | passive: listens for beacons and probe responses |
| Transmits? | **yes**, probe requests (ordinary station behaviour) | **no, never** |
| Refresh | one scan every 15 s, 2–4 s per scan | one 13-channel sweep every ~3.25 s |
| Finds | more networks, faster | fewer networks, more slowly |
| Encryption info | from the driver | derived from the beacon — see [the divergence table](#where-promiscuous-mode-and-the-scan-disagree) |
| Deauth counting | no | yes |

## How it is built

```
ScanSource     ──┐
   (WiFi scan)   ├─fills─> NetList ──reads──> view_oled    (128×64, paginated)
PromiscSource  ──┘         (model)      └──> view_serial   (full SSID + BSSID)
   (dot11 parser)                              ▲
                              WiFi_scanner.ino ─┘  decides WHEN, never HOW
```

| File | Role |
|---|---|
| [`netlist.h`](netlist.h) / [`.cpp`](netlist.cpp) | The model. A fixed-size array of networks, permanently sorted by signal strength. Knows nothing about WiFi, the display, or the serial port. |
| [`netsource.h`](netsource.h) | The interface a data source implements: `begin()`, a non-blocking `poll()`, read-only access to the model, and two short strings the views print without interpreting. |
| [`scansource.h`](scansource.h) / [`.cpp`](scansource.cpp) | Asynchronous `WiFi.scanNetworks()` wrapped in a two-state machine. |
| [`dot11.h`](dot11.h) / [`.cpp`](dot11.cpp) | Pure 802.11 frame parsing: frame control bits, tagged parameters, RSN and WPA elements, deauth bodies. No state, no heap, no output. |
| [`promiscsource.h`](promiscsource.h) / [`.cpp`](promiscsource.cpp) | Promiscuous capture: the RX callback, the queue, channel hopping, the BSSID roster, deauth counters. |
| [`view_oled.h`](view_oled.h) / [`.cpp`](view_oled.cpp) | Owns the U8g2 instance and does all drawing. Contains no capture code. |
| [`view_serial.h`](view_serial.h) / [`.cpp`](view_serial.cpp) | The second view over the same model. Prints full, untruncated SSIDs plus BSSID. |
| [`config.h`](config.h) | Source selection, capacities and timing, with the derivation of every number. |
| [`WiFi_scanner.ino`](WiFi_scanner.ino) | Glue and clocks: page timer, redraw decisions. No `delay()`. |

**The seam is `netsource.h`, and it is there on purpose.** The rule the layout
enforces is that a module which acquires data never prints anything, and a module
which prints never acquires anything. Neither source contains a `Serial.print`;
neither view mentions `WiFi`. Swapping the source is one `#define`.

A concrete consequence of taking that seriously: the model does not store
`wifi_auth_mode_t` from ESP-IDF, because that would force both views to
`#include <WiFi.h>` and so depend on the data source. Instead the model defines
its own small `AuthKind` enum, and each source translates into it — `scansource`
from the driver's answer, `dot11` from the beacon's own bytes. `scansource.cpp`
is the only file in the project where a WiFi-stack type appears.

The two short strings on `NetSource` — `status()` and `diagnostics()` — are one
pattern used twice, not a pattern plus an exception. Each is a compromise, and
[`netsource.h`](netsource.h) spells out both halves of it: the view stops knowing
what "busy" means, but the source starts producing text for a display. The
alternative (structured state, formatted by the view) simply moves the coupling
back the other way.

Code comments are in Czech; identifiers and this README are in English.

## Decisions worth explaining

### Reporting the weakest accepted method

A network in WPA/WPA2 mixed mode still serves WPA clients. Whoever wants in
takes the weaker path, and the fact that the AP also speaks WPA2 does not get in
their way. Reporting that network as `WPA2` would describe its security as
better than it is — for a survey tool, that is the one direction of error that
matters. So every network is reported by **the weakest method it accepts**, and
mixed modes get their own markers instead of collapsing into the stronger
generation.

| Marker | Meaning |
|---|---|
| `-`  | open, no encryption |
| `o`  | OWE / Enhanced Open — encrypted, but no password and no authentication |
| `w`  | WEP |
| `1`  | WPA only |
| `2`  | WPA2 only |
| `3`  | WPA3 only |
| `12` | mixed — accepts WPA as well as WPA2 |
| `23` | mixed — accepts WPA2 as well as WPA3 |
| `e1` | Enterprise (802.1X) at the WPA generation |
| `e2` | Enterprise at the WPA2 generation |
| `e3` | Enterprise at the WPA3 generation, including Suite-B 192-bit |
| `?`  | could not be determined |

The column is two characters wide plus a full character of space before the
name, and both cost something: the name column went from 13 characters to 11.
One character bought the second digit, one bought the separator. The separator
is not cosmetic — without it `12TP-LINK` reads as one word and `2O2 INTERNET`
reads as `202 INTERNET`.

Two digits side by side read as "accepts both" without consulting a legend,
which for a tool someone glances at for five seconds is worth more than a
character of name. The same scheme covers Enterprise: `e1`/`e2`/`e3` keeps the
difference between WPA-Enterprise and WPA3-Enterprise visible where a single `e`
would have hidden it, and it means WPA3-Enterprise Suite-B 192-bit does not have
to be under-reported just to fit. WPA3-Enterprise Transition Mode reports as
`e2`, because WPA2-Enterprise is the weakest generation it accepts. The scheme
extends itself: another mixed mode is written by the same rule, with no new
letter to invent.

One mode is deliberately *not* treated as mixed. `WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE`
reports as `3`, because the ESP-IDF header states it yields the same result as
`WIFI_AUTH_WPA3_PSK`. The identifier suggests otherwise, but a documented
statement outranks an inference drawn from a name; the doubt is recorded in a
comment at the mapping site rather than acted on.

The mapping was written against `esp_wifi_types_generic.h` from core 3.3.11,
which defines seventeen auth modes — including several an obvious implementation
misses, such as `WPA_ENTERPRISE` and the transition modes. Note also that
`WIFI_AUTH_WPA2_ENTERPRISE` is an alias for `WIFI_AUTH_ENTERPRISE` in that
header, so only one of the two names can appear as a `case` label.

### Where promiscuous mode and the scan disagree

In scan mode the encryption comes from the driver. In promiscuous mode it is
derived from the beacon's own Capability field, RSN element and WPA vendor
element. Those two paths do not always land on the same answer, and without this
table the `e2` below would be a claim nobody could check.

| Case | Scan | Promiscuous | Why |
|---|---|---|---|
| WPA3-Enterprise, not 192-bit | `e3` | `e2` | The AKM selectors are identical to WPA2-Enterprise (1 or 5). The only difference is *required* management frame protection, which a WPA2 network may legitimately enable too. Telling them apart would be a guess. |
| WPA3-Enterprise Suite-B 192-bit | `e3` | `e3` | AKM 12 is unambiguous. |
| WEP | `w` | `w` | The scan gets it from the driver; here it is **inferred**. No beacon element announces WEP — it is recognised only as "Privacy set, no WPA-family element". |
| RSN present, AKM list unreadable | correct | `?` | No guessing. |
| Truncated or corrupt frame | correct | `?` | The driver assembles its answer from repeated attempts; this reads one frame. |
| WAPI, DPP | `?` | `?` | Neither can name them. |
| Hidden SSID | `<hidden>` | `<hidden>` | But a probe response sometimes reveals the name, so promiscuous mode can see **more**. |
| SSID with an embedded NUL | truncated | complete | Promiscuous mode reads the SSID element with its own length field; the scan only gets a C string. |
| 5 GHz networks | invisible | invisible | The ESP32-WROOM-32D has no 5 GHz radio. |

### Asynchronous scan instead of a blocking one

`WiFi.scanNetworks()` with no arguments does not return until the scan finishes.
A scan visits all 13–14 channels and spends a couple of hundred milliseconds on
each, so `loop()` stalls for 2–4 seconds. Nothing else can happen in that
window: pages do not advance, the display is frozen for roughly a fifth of the
time.

`WiFi.scanNetworks(true, true)` returns immediately (the second `true` also
requests hidden networks). The scan proceeds in the WiFi task and the caller
polls `WiFi.scanComplete()`, which returns `-1` while running, `-2` on failure,
or the number of networks found. The cost is a two-state machine, entirely
contained in `scansource.cpp`; the sketch calls `poll()` and never learns that a
state machine exists.

Two details that are easy to miss: `WiFi.scanDelete()` has to be called after
reading the results, or they sit in the WiFi stack's memory until the next scan;
and there is a timeout guard, so a scan that never reports completion cannot
wedge the state machine permanently.

Time comparisons are written as `millis() - mark >= period`, never
`millis() > mark + period`. `millis()` wraps after about 49 days, and the second
form breaks for days when it does. Unsigned subtraction survives the wrap.

### Channel dwell of 250 ms

The radio hears one channel at a time, so promiscuous mode sweeps channels 1–13
in turn. A beacon interval is typically 100 TU = 102.4 ms, and a window of
length *L* is guaranteed to contain at least `floor(L / 102.4)` beacons from
each audible network regardless of how the phases line up:

| Dwell | Guaranteed beacons | Full sweep |
|---|---|---|
| 150 ms | 1 | 1.95 s |
| 200 ms | 1 | 2.60 s |
| **250 ms** | **2** | **3.25 s** |
| 300 ms | 2 | 3.90 s |

One chance is not enough — beacons are delayed by collisions and a weak AP is
sometimes not heard at all. Two is a reasonable minimum, and 250 ms is the
shortest dwell that guarantees two; 300 ms buys no more certainty, only a longer
sweep. As a side effect the 3.25 s sweep is close to the duration of one
`scanNetworks()`, so the display's tempo does not change between modes.

The sweep is also the unit of publication: the list is handed to the views once
per completed sweep, which is the promiscuous equivalent of a finished scan.

### No `String` in the data model

`NetInfo` stores the SSID as `char[33]` — 32 bytes maximum per 802.11, plus a
terminator. `WiFi.SSID()` hands back a `String`, so the data is copied into the
fixed array immediately and the `String` is dropped; nothing heap-allocated
outlives that one line.

The reason is fragmentation, not raw size. A device that runs for days and
rewrites its network list every few seconds will chop the heap into unusable
pieces, and the allocation that eventually fails will do so at the worst
possible moment. Fixed arrays live in `.bss`: their size is known at compile
time and they cannot fail at runtime. `sizeof(NetInfo)` is 48 bytes and the
whole list is about 1.2 kB, against roughly 300 kB of DRAM free once the WiFi
stack is up.

### SSID sanitisation at the trust boundary

An SSID is up to 32 arbitrary bytes transmitted by someone else's hardware.
Nothing guarantees it is text. An AP can be named with ANSI escape sequences,
and those would let a stranger recolour, erase or reposition lines in the
terminal reading the serial output — debug output you cannot trust is worse than
none. On the display, bytes above 0x7E render as blanks anyway, because the
`_tf` fonts cover Latin-1 and no more.

So `setSsidSanitized()` keeps only printable ASCII (0x20–0x7E) and replaces
everything else with `.`. It lives with the model rather than in either source,
because it is what makes one rule hold everywhere: **anything in `NetList` is
safe to print.**

An empty SSID stays empty. Turning it into `<hidden>` is a display decision, so
it belongs in the views, and each view is free to word it differently. A hidden
network that pads its SSID element with zero bytes rather than sending an empty
one is recognised and treated the same way — otherwise it would appear as a row
of dots.

### Sorted insertion instead of a separate sort

`NetList::add()` puts each network directly into its place by RSSI. Three things
fall out of one piece of code: the ordering itself; the behaviour when the array
is full, where the weakest entry simply drops off the end; and an API shaped for
a source that produces networks one at a time rather than as a finished list —
which is exactly what promiscuous mode turned out to need. Insertion sort over
24 elements is at worst about 290 comparisons, nothing at 240 MHz. Ties keep
their previous order, so networks with equal RSSI do not swap places for no
reason.

Promiscuous mode adds `upsertByBssid()`, which updates an existing entry in
place, and `expireOlderThan()`, because entries that arrive one beacon at a time
would otherwise never leave: a network that moves away would sit in the list
forever. Entries not heard for 30 seconds are dropped, which is about nine
sweeps — short enough to be current, long enough that missing a weak AP once or
twice does not make it flicker.

### Showing what was dropped

`MAX_NETS` is 24. The limit is not memory — it is legibility, since 24 networks
is five pages, and at three seconds a page a full cycle already takes as long as
one refresh.

But an array that silently drops the weakest entries makes the header lie: `24`
would mean "24 networks exist" when it really means "24 fit". So `NetList`
tracks how many networks the source knows about alongside how many it kept. When
they differ, the header reads `24/37 AP`; when everything fits, one number.

Promiscuous mode cannot count that the way a scan does — the same network
arrives over and over, beacon after beacon — so it keeps a **roster of BSSIDs**
heard, independent of what fits in the list. And when the roster itself fills
up, it stops knowing too, so the header reads **`24/96+`**: the `+` means *at
least*. The same `+` appears wherever a number is a lower bound rather than a
measurement, including deauth counts. When the tool does not know how much it
does not know, it says so rather than printing a confident number.

### Counting deauthentication frames

In promiscuous mode the tool also counts **deauthentication** (management
subtype 12) and **disassociation** (subtype 10) frames. Both carry a two-byte
reason code immediately after the 24-byte header, and both pass the same
management-frame filter the beacons do.

A single deauth frame is not remarkable. They are sent perfectly legitimately —
a client leaving, an AP dropping an idle station. What is worth surfacing is a
**burst**, so the tool counts frames per BSSID and applies a threshold.

| Constant | Value | Derivation |
|---|---|---|
| `DEAUTH_BURST_MIN` | 5 | Legitimate deauth is a one-off; with retries it tops out around three per 250 ms window from one BSSID. Five leaves margin above that while still catching a flood as slow as 20 frames/s (5 ÷ 0.25 s). |
| `DEAUTH_BCAST_MIN` | 2 | A broadcast deauth (Address 1 = `ff:ff:ff:ff:ff:ff`) disconnects every client at once. One arrives legitimately when an AP restarts; two in a quarter second is not a restart. A stronger signal earns a lower threshold. |
| `DEAUTH_HOLD_MS` | 60000 | A burst is over in seconds. If the marker cleared with it, you would only ever see it by watching. A minute is long enough to notice and short enough to distinguish "happening now" from "happened". |
| `DEAUTH_SLOTS` | 12 | A burst targets one or two networks; the rest is headroom for legitimate deauth from nearby APs during the hold. |

Thresholds are evaluated per sweep, which means per 250 ms window on that
channel.

**The count is always a lower bound.** Each channel is heard for 250 ms out of
every 3250 ms, about **7.7 % of the time**. A burst is therefore mostly missed.
The tool does **not** multiply the count by thirteen to compensate, because the
correction factor is unknowable: if the burst lasted five seconds we saw an
eighth of it, and if it lasted 300 ms and we happened to be listening we saw
nearly all of it — and there is no way to tell which happened. So the number is
reported as measured, marked `+`, with the duty cycle stated next to it.

#### What the marker means

> **The marker means the network was *named in* deauth frames. It does not mean
> the network sent them.**

The sender's MAC address in a deauth frame is trivially forged — that is the
whole mechanism by which networks get disrupted. The tool knows **what name the
frame was signed with**, not who transmitted it. Either way the frames concern
that network's clients, so marking that row is correct in both cases; what would
be wrong is reading the marker as blame.

The serial output repeats this on every burst, along with whether the frame was
signed as the AP itself (Address 2 = Address 3) and whether the network
**requires management frame protection** (802.11w). That last one changes what
the burst means: against a network with protection required, forged deauth
frames have no effect on clients — the tool still sees and counts them.

On the display it appears as two things, both costing zero pixels on a layout
with none to spare:

- the header status changes from `P6` to `!6`, visible on every page;
- the affected network's row is drawn inverted, when that page is showing.

The vocabulary throughout is deliberate. The tool observes a burst of deauth
frames, which is a phenomenon and not a verdict — so the wording everywhere
stops at what was measured, names no culprit and infers no intent.

## Deliberately not done

- **Camping on a channel when a burst is detected.** Staying put would see much
  more of the burst. It was rejected because it makes the tool's scheduling an
  input someone else controls: anyone wanting to blind it on twelve channels
  need only give it a reason to stare at the thirteenth. It would also distort
  ageing — networks on other channels would stop being heard and start expiring
  — and it would destroy the one clean sentence that makes the numbers
  interpretable, that every channel is heard for the same 7.7 % of the time.
  A bounded version (extend one dwell, once per sweep) was considered and left
  out too: a knob wired to zero is a knob somebody eventually turns on because
  it sounds like an improvement, long after the reason not to is forgotten.
- **A dedicated deauth screen.** The header says *that* it happened and the
  inverted row says *which* network; the serial output carries the detail.
  A third page in the rotation was judged not worth its cost yet.
- **A counter that decays across sweeps.** The biggest blind spot below is slow,
  targeted deauth, and the known remedy is a per-BSSID counter that decays over
  sweeps rather than resetting: two frames per second never reaches a per-window
  threshold, but accumulates over ten sweeps where legitimate traffic does not.
  It is not implemented because the same accumulation makes APs that deauth
  clients as a matter of policy — band steering, load balancing — cross the
  threshold routinely.
- **Hash tables.** The network list, the BSSID roster and the deauth slots are
  all linear scans over small fixed arrays. At 24, 96 and 12 entries a scan is a
  few dozen byte comparisons, while a hash table would add collision handling
  and a failure mode that is harder to reason about — and one of these is
  scanned from inside a callback where predictable cost matters more than
  average cost.

## Known limitations

### Detection

- **Slow, targeted deauth is invisible.** Two frames per second aimed at one
  client stays under the per-window threshold indefinitely. This is the largest
  blind spot and no threshold setting fixes it — a lower one would collide with
  legitimate traffic. See the decaying counter above.
- **A burst on another channel is missed entirely.** With a 7.7 % duty cycle per
  channel, most of what happens is never heard. A burst can begin and end
  between two visits.
- **A burst split across windows can slip through.** Three frames now and three
  next sweep are each below the threshold.
- **False positives are expected.** An AP restarting, changing configuration or
  updating firmware sends legitimate broadcast deauth. Enterprise APs deauth
  clients on purpose for band steering and load balancing. A failing AP drops
  clients repeatedly. None of these are distinguishable from the outside.
- **Bursts against 802.11w networks are counted even though they do nothing.**
  Where management frame protection is required, forged deauth frames are
  rejected by clients. The tool still sees them; the serial output says when the
  network required protection, but the display marker does not distinguish.
- **The deauth slot table holds 12 BSSIDs.** Beyond that the least recently
  active slot is recycled and its accumulated counts are lost. This is reported
  on serial, never silently.
- **The BSSID roster holds 96 entries.** Beyond that `seen()` becomes a lower
  bound and the header shows `+`. Reported, not hidden.
- **A flagged network can leave the list before its flag expires.** Entries age
  out after 30 s but the deauth flag holds for 60 s. The header keeps showing
  `!` and the serial output keeps the detail, but there is no row left to
  invert. Refreshing `lastSeenMs` on a deauth frame would fix the display and
  would also be a lie — it would claim a beacon was heard that was not.

### Capture and display

- **Hidden networks are unreliable.** An AP that does not broadcast its name may
  also decline to answer an active scan and never appear. In promiscuous mode a
  probe response sometimes reveals it. That is 802.11 behaviour, not a bug here.
- **The order fluctuates.** RSSI moves by a few dB between readings, so networks
  of similar strength trade places. Sorting is stable, so the jitter is no worse
  than the signal's own.
- **Frames dropped inside the driver are invisible.** The tool counts what it
  fails to queue, but frames the WiFi driver discards before the callback runs
  cannot be seen or counted at all.
- **The display does not show the original SSID bytes.** Sanitisation is
  deliberate, but a network named with non-ASCII characters shows up with dots.
  The raw bytes are not preserved anywhere.
- **In scan mode an embedded NUL byte truncates the SSID.** `WiFi.SSID()`
  returns a C string, so anything after a zero byte is gone before this code
  sees it. Promiscuous mode does not have this problem.
- **No diacritics.** `u8g2_font_6x10_tf` covers ASCII and Latin-1; `ě š č ř ž ů`
  are Latin-2 and render as blanks. All on-screen strings are plain ASCII.
- **Long SSIDs are cut off.** 128 px at 6 px per character leaves 11 characters
  for the name once encryption, channel and dBm are accounted for. Truncated
  names end in `~`; the serial output has the full name.
- **Not every encryption mode has been seen in the field.** Modes needing
  uncommon hardware — OWE, WPA3-Enterprise 192-bit, WPA-Enterprise, WEP — are
  derived from the header definitions rather than confirmed against a live
  network.
- **2.4 GHz only.** The ESP32-WROOM-32D has no 5 GHz radio.
- **The two modes cannot run at once.** The radio does one or the other, which
  is why the source is chosen at compile time.

## License

MIT — see [LICENSE](LICENSE).

## Notes

Written with [Claude Code](https://claude.com/claude-code).
