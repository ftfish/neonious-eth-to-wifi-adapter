# neonious one → Ethernet-to-WiFi adapter

Firmware that repurposes an old **neonious one** board (ESP32-D0WDQ6 + LAN8720A
Ethernet PHY) into a small **Ethernet-to-WiFi NAT router**: plug an
Ethernet-only device into the RJ45 jack and it reaches the internet over your
WiFi network.

The **neonious one** was a JavaScript-programmable IoT microcontroller with
built-in Ethernet. For background on the original board see
[robotzero.one's review](https://robotzero.one/neonious-javascript-microcontroller/),
and the vendor's own (now archived) code lives at
[github.com/neonious](https://github.com/neonious). This project ignores the
original firmware and runs a fresh Arduino-ESP32 sketch on the bare hardware.

- Firmware: [`firmware/EthToWifiBridge/EthToWifiBridge.ino`](firmware/EthToWifiBridge/EthToWifiBridge.ino)
- Reproducible build/spec + debugging history: [`docs/spec.txt`](docs/spec.txt)

---

## What it does

```
[ Ethernet device ] --RJ45--> [ neonious one ] ~~WiFi STA~~> [ your router ] --> Internet
   192.168.5.x (DHCP)          ETH gw 192.168.5.1            gets a DHCP lease
                               runs DHCP server + NAPT        on your LAN
```

The board joins your WiFi as a **station**, runs a **DHCP server** and **NAT
(NAPT)** on the Ethernet side, and routes the Ethernet device's traffic out over
WiFi. The device lives on its own subnet (`192.168.5.0/24` by default).

> **Why NAT, not a transparent bridge?** The ESP32 WiFi stack in station mode
> can't L2-bridge a foreign device's MAC onto the WiFi link, so we route at L3
> with NAPT. The Ethernet device is therefore **behind NAT** — fine for normal
> internet use, but it is not on your main LAN's broadcast domain (no mDNS/SSDP
> discovery across the boundary).

## Features

- WiFi station uplink with credentials stored in NVS
- DHCP server + NAPT on the Ethernet port (with the upstream DNS handed to clients)
- **Web UI** (port 80) to reconfigure everything without reflashing
- **OTA** firmware updates (ArduinoOTA)
- **In-RAM web log** (the USB serial console is unavailable — see quirks)
- **Live traffic statistics** (RX/TX bytes & packets, uptime, clients, RSSI, …)
- **Recovery AP** so you're never locked out if WiFi credentials are wrong
- **Triple-tap RESET → factory reset** out-of-band escape hatch (works even when
  WiFi client isolation blocks the web UI)

---

## ⚠️ Reverse-engineered hardware quirks (read this!)

This board's wiring is unusual and the original reverse-engineered notes were
partly wrong. The table below is the **corrected, verified** configuration.

| Function | Pin | Notes |
|---|---|---|
| RMII REF_CLK | **GPIO0 (input)** | Clock comes from an **external 50 MHz oscillator** |
| Oscillator **enable** | **GPIO5** | Passed as the ETH "power" pin; driver drives it HIGH after boot |
| MDC (SMI clock) | **GPIO1** | ⚠️ also UART0 TX — **USB serial dies once Ethernet starts** |
| MDIO (SMI data) | **GPIO23** | (non-default; many examples use GPIO18) |
| PHY SMI address | **auto (-1)** | Not 0 or 1; let the driver scan the MDIO bus |
| RMII data | GPIO19/21/22/25/26/27 | Fixed ESP32 EMAC pins, not changeable |
| Flash voltage | GPIO12 strap = 1.8 V | **Do not drive GPIO12** |

### The big one: clock configuration

The CPU is an **ESP32-D0WDQ6 rev v1.0** (old silicon). Its EMAC **RX path is
unreliable when the ESP32 generates the RMII clock**. This board provides an
**external 50 MHz oscillator** whose enable line is **GPIO5**, gated off at boot
(GPIO0 doubles as a boot-strap pin, so a free-running clock there would break
booting).

Correct setup → drive **GPIO5 high** and take the clock as an **input on GPIO0**:

```cpp
ETH.begin(ETH_PHY_LAN8720, /*addr*/-1, /*MDC*/1, /*MDIO*/23,
          /*power*/5, ETH_CLOCK_GPIO0_IN);
```

Symptom if you get this wrong: with `ETH_CLOCK_GPIO0_OUT` the PHY *powers up,
LEDs light, link negotiates at 100/full, and MDIO works* — but **no data passes**
(ping/DHCP fail). That "link OK but zero data" signature is the rev-v1.0 clock
issue.

### No serial console

MDC is on **GPIO1 (UART0 TX)**, so once `ETH.begin()` runs, USB serial output
stops. **Use the web log** at `http://<board-ip>/log` for all diagnostics.

---

## Build & flash

### Toolchain
- **Arduino-ESP32 core 3.3.x** (`esp32:esp32`)
- `arduino-cli` (or the Arduino IDE), board **"ESP32 Dev Module"**, FQBN
  `esp32:esp32:esp32`, default 4 MB partition scheme (has dual OTA slots)
- `python` + `esptool` (first flash), `espota.py` (OTA; ships with the core)

### Configure defaults
No WiFi credentials are compiled in — flash as-is and the board comes up as a
**setup AP** on first boot (see [First-time setup](#first-time-setup)). You can
optionally pre-set the OTA password or subnet via the `DEF_*` macros at the top
of the sketch, but secrets are meant to be entered through the web UI.

### Compile
```bash
arduino-cli compile --fqbn esp32:esp32:esp32 --output-dir build firmware/EthToWifiBridge
```

### Flash over USB (first time) — wiring a USB-to-TTL adapter

This board has **no USB-to-serial bridge**. To do the first flash you connect a
**3.3 V USB-to-TTL (USB-to-UART) adapter** directly to the ESP32-WROVER
module's UART0 pins and put the chip into download mode. Module pinout
reference: <https://www.espboards.dev/esp32/esp32wrover/>.

> ⚠️ Use a **3.3 V** adapter (or one with the I/O jumper set to 3.3 V). 5 V on
> these pins can damage the ESP32. Tie the adapter's **GND to the board GND**.

| USB-TTL adapter | → | Module pin (silkscreen) | ESP32 GPIO | Notes |
|---|---|---|---|---|
| **TXD** | → | **RXD0** | GPIO3 (U0RXD) | adapter transmit → ESP32 receive |
| **RXD** | → | **TXD0** | GPIO1 (U0TXD) | ESP32 transmit → adapter receive (also MDC at runtime) |
| **GND** | → | **GND** | — | common ground (required) |
| 3V3 *(optional)* | → | **3V3** | — | only if powering the board from the adapter |
| IO0 *(optional)* | → | **IO0** | GPIO0 | only if using esptool DTR/RTS auto-reset |

**Entering download mode.** The board has an on-board **RESET** button, so you
don't need to wire EN: hold **GPIO0 (IO0) to GND**, press and release the
**RESET button**, then release GPIO0. The chip is now in the serial bootloader.
(esptool's automatic reset also works if your adapter's DTR→IO0 / RTS→EN are
wired, but the manual button method is simpler here.)

> GPIO0 doubles as the RMII clock input at runtime, but the external oscillator
> is gated off at boot (its enable, GPIO5, is low), so GPIO0 is free to act as
> the normal boot-strap pin during flashing.

Then flash the compiled binary:
```bash
python -m esptool -p <COM> write_flash 0x10000 build/EthToWifiBridge.ino.bin
# (or use the Arduino IDE upload over the same COM port)
```
Note: there is **no runtime serial console** (GPIO1 = MDC once Ethernet starts);
the UART is only usable for flashing and the early boot banner. Use `/log`
afterward. After this first flash, all later updates can go over [OTA](#flash-over-the-air-ota).

### Flash over the air (OTA)
```bash
python <core>/tools/espota.py -i <board-ip> -p 3232 -a <ota-password> \
    -f build/EthToWifiBridge.ino.bin -r
```
ArduinoOTA listens on **UDP 3232** (a TCP probe of 3232 showing "closed" is
normal). Default OTA password is `admin`, hostname `neonious-eth-wifi`.

---

## Using it

### First-time setup

The firmware ships with **no WiFi credentials**, so on first boot (and after a
factory reset) it starts a setup access point:

1. Connect to WiFi **`neonious-setup`** (password `neonious`) and open
   `http://192.168.4.1/` (login `admin` / password `admin`).
2. Enter your WiFi SSID + password (and change the OTA/web password), then
   **Save & reboot**.
3. The board now joins your WiFi. Find its IP (check your router) and reopen
   `http://<board-ip>/`.
4. Plug your Ethernet device into the RJ45. It gets a `192.168.5.x` address,
   gateway/DNS `192.168.5.1`/upstream DNS, and internet via WiFi.

> **Change the defaults.** The setup-AP password and the `admin` OTA/web
> password are shipped defaults — set your own from the web UI.

### Web UI
| Path | Purpose |
|---|---|
| `/` | Settings form + live status + **traffic stats** |
| `/log` | Live auto-refreshing log viewer |
| `/log.txt` | Raw log text (for `curl`) |
| `/stats.json` | Traffic/uptime/system stats (JSON) |
| `/scan` | WiFi scan (used by the SSID picker) |
| `/save` | Save settings + reboot |
| `/reset` | Erase all settings + reboot to defaults |

All pages require HTTP Basic auth: user `admin`, password = the OTA password.

Configurable from the UI (persisted to NVS): WiFi SSID/password, Ethernet
gateway IP, subnet mask, DHCP range, OTA/web hostname and password.

### Stats shown
Link speed/duplex, RX/TX bytes & packets, uptime, NTP wall-clock boot &
current time, DHCP client count + last lease, WiFi RSSI/SSID/IP, LAN gateway,
free heap, and firmware build timestamp.

### Example boot log
A healthy startup looks like this (from `/log.txt`; addresses/SSID redacted):

```text
[00:00:00] boot: neonious Ethernet -> WiFi adapter
[00:00:00] boot tap 1/3 - tap RESET again within 8s to factory-reset
[00:00:00] config loaded, target SSID 'YourWiFi'
[00:00:00] WiFi: connecting to 'YourWiFi'...
[00:00:00] WiFi: associated to AP
[00:00:01] WiFi: got IP 192.168.1.50
[00:00:01] WiFi: connected, IP 192.168.1.50
[00:00:01] WiFi: gw 192.168.1.1, dns 8.8.8.8 / 8.8.4.4
[00:00:01] OTA ready: host 'neonious-eth-wifi'
[00:00:01] ETH: starting LAN8720 (PHY addr -1, clk GPIO0_IN, MDC=1 MDIO=23)...
[00:00:03] ETH: interface started
[00:00:03] ETH: link up
[00:00:03] router: set_ip 192.168.5.1 -> ESP_OK
[00:00:03] router: dhcp pool 192.168.5.100 - 192.168.5.199
[00:00:03] router: offering upstream DNS 8.8.8.8 + 8.8.4.4 to clients
[00:00:03] router: dhcps_start -> ESP_OK
[00:00:03] router: napt_enable -> ESP_OK
[00:00:03] ETH.begin() = OK (PHY found on MDIO)
[00:00:03] router: set_ip 192.168.5.1 -> ESP_OK
[00:00:03] router: dhcp pool 192.168.5.100 - 192.168.5.199
[00:00:03] router: offering upstream DNS 8.8.8.8 + 8.8.4.4 to clients
[00:00:03] router: dhcps_start -> ESP_OK
[00:00:03] router: napt_enable -> ESP_OK
[00:00:03] ETH: link UP (100 Mbps, full)
[00:00:08] reset-tap counter cleared (normal uptime reached)
```

The `router:` block appears twice because `startEthRouter()` runs once from the
`ETH_CONNECTED` event and once from `setup()` after `ETH.begin()` returns with
the link already up — both are idempotent. The two `ETH: link …` lines are the
event-driven and polled link-up reports (the polled one adds speed/duplex).

### Setup / recovery AP
With no WiFi configured, or if the saved WiFi can't be joined within 20 s, the
board starts an access point **`neonious-setup`** (password `neonious`) at
**`http://192.168.4.1`** so you can enter/fix the configuration or OTA a new
build.

### Factory reset (triple-tap RESET)
If you can't reach the web UI over WiFi — e.g. the network uses **client
isolation**, so devices on it can't talk to the board — you can still wipe all
settings using only the on-board **RESET** button:

> **Press RESET 3 times in a row**, pausing ~1–2 s between taps (let the board
> begin booting each time, but tap again within ~8 s). On the third boot the
> board erases all saved settings and comes up as the `neonious-setup` AP.

A boot counter in NVS tracks the taps; it auto-clears after the board has been
up for ~8 s, so a normal power-cycle or single reset never triggers it. (You can
also always reach `http://192.168.5.1/` from a device plugged into the RJ45 — the
wired side is unaffected by WiFi client isolation.)

---

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| Ethernet LEDs off, no link | Oscillator not enabled — power pin must be **GPIO5**, clock `GPIO0_IN` |
| Link up but no ping/DHCP | Wrong clock mode (must be external `GPIO0_IN`, not `GPIO0_OUT`) |
| `ETH.begin() = FAILED` in log | PHY not detected — keep PHY address on auto (`-1`) |
| Client gets `192.168.5.2` not `.100` | DHCP range > 100 addresses; keep it ≤ 100 (lwIP `DHCPS_MAX_LEASE`) |
| Client DNS = `192.168.5.1`, can't resolve | Old lease; renew DHCP on the client (`ipconfig /release && /renew`) |
| Board reboot-loops after a bad flash | **Unplug the Ethernet cable** (no link → no crash), then OTA a fix |
| Can't reach web UI over WiFi (client isolation) | **Triple-tap RESET** to factory reset, or browse `http://192.168.5.1/` from a device on the RJ45 |
| No serial output | Expected — MDC is on GPIO1/TX; use `/log` |

---

## How it works (implementation notes)

These are the non-obvious software details (full detail in [`docs/spec.txt`](docs/spec.txt)):

- **DHCP server on Ethernet:** the esp_netif ETH interface has no DHCP-server
  instance (`esp_netif_dhcps_start()` fails on it), so the firmware runs lwIP's
  `dhcps_*` **directly on the ETH netif** via `esp_netif_get_netif_impl()`.
- **Thread safety:** lwIP `dhcps_*` are not thread-safe; they're called inside
  the TCP/IP thread via `esp_netif_tcpip_exec()` (calling them from the event/
  loop task panics the board).
- **DNS:** the `OFFER_DNS` flag must be set or clients get the server's own IP as
  DNS. Bringing ETH up wipes lwIP's global DNS, so the upstream DNS is **cached
  at WiFi-connect time** and both primary + secondary are offered.
- **NAPT** is enabled on the ETH netif; the WiFi STA is the default route.
- **Traffic counters** wrap the ETH netif's `input`/`linkoutput` pointers
  (lwIP `MIB2_STATS` is off in this build).

---

## Default configuration summary

| Setting | Value |
|---|---|
| Ethernet gateway / subnet | `192.168.5.1` / `255.255.255.0` |
| DHCP pool | `192.168.5.100 – 192.168.5.199` |
| DNS offered to clients | upstream DNS from WiFi (primary + secondary) |
| OTA hostname / password | `neonious-eth-wifi` / `admin` |
| Web UI login | `admin` / OTA password |
| WiFi credentials | none compiled in — set via the setup AP on first boot |
| Setup / recovery AP | `neonious-setup` / `neonious` → `192.168.4.1` |
