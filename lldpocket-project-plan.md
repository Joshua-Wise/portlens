# LLDPocket — DIY Pocket Ethernet Tester

## Project Overview

**LLDPocket** (LLDP + Pocket) is a pocket-sized, open-source network testing tool that replicates the core functionality of commercial tools like Netool, LinkSprinter, and Pocket Ethernet. Plug into any Ethernet jack to instantly identify the switch port, VLAN, IP assignment, and connectivity status.

**Target cost:** ~$40
**Target form factor:** Phone-sized, 3D-printed enclosure
**Platform:** ESP32 running ESP-IDF v5.3
**Build target:** Olimex ESP32-PoE-ISO (WROOM variant with PSRAM)

---

## Hardware

### Primary Board: Olimex ESP32-PoE-ISO

- **MCU:** ESP32-D0WD (Xtensa dual-core, 240MHz) with 8MB PSRAM
- **Ethernet PHY:** LAN8710A (RMII interface — supports raw frame capture)
- **PoE:** IEEE 802.3af compliant via TPS2375PW (37V+ DC input)
- **Galvanic isolation:** 3000VDC between Ethernet power and board
- **Built-in:** LiPo connector + charger, user-programmable button, reset button
- **Connectivity:** WiFi 802.11 b/g/n, BLE 4.2
- **USB:** Micro-USB for programming/debugging (CH340 USB-serial)

### Display Module: Elecrow 4.0" Capacitive Touch SPI LCD

- **Display:** ST7796S 320x480, 65K colors, SPI interface
- **Touch:** FT6336U capacitive touch controller, I2C
- **Extras:** Built-in SD card slot (used for logging)
- **Dimensions:** 108.0mm x 60.88mm

### Pin Assignments (Olimex ESP32-PoE-ISO)

| Function | GPIO | Notes |
|---|---|---|
| ETH MDC | 23 | |
| ETH MDIO | 18 | |
| ETH PHY Power | 12 | HIGH=ON (inverted by NPN Q2/Q3) |
| ETH RMII CLK | 0 | EXT_IN (WROVER/PSRAM boards) |
| TFT MOSI | 14 | SPI2_HOST |
| TFT SCK | 33 | SPI2_HOST |
| TFT CS | 5 | 10k pull-up |
| TFT DC | 2 | Strapping pin (LOW at boot) |
| TFT RST | 15 | 10k pull-up |
| SD CS | 32 | Shares SPI bus with TFT |
| SD MISO | 39 | Input-only (SVN), fine for MISO |
| Touch SDA | 13 | I2C, 2.2k pull-up |
| Touch SCL | 4 | I2C |
| Touch INT | 36 | Input-only (unused, polling mode) |
| Button | 34 | Onboard BUT1, input-only, 10k pull-up |

### Power Architecture

- **PoE mode (default):** Board powers from Ethernet cable via 802.3af.
- **Battery mode:** Slide switch connects LiPo to Olimex onboard battery input. Used for non-PoE jacks.
- **Charging:** When PoE is active and battery is connected, the Olimex onboard charger can trickle-charge the LiPo.

---

## Firmware Architecture

### Framework

- **ESP-IDF v5.3** (Espressif IoT Development Framework)
- Rationale: Full control over raw Ethernet frame reception, PHY configuration, and low-level networking.

### Project Structure

```
lldpocket/
├── CMakeLists.txt
├── sdkconfig
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                  # App entry, state machine
│   ├── config.h                # Board pin definitions, feature flags
│   ├── eth/
│   │   ├── eth_init.c/.h       # Ethernet PHY init (LAN8710A)
│   │   └── raw_frame.c/.h      # Raw Ethernet frame capture callback
│   ├── protocols/
│   │   ├── lldp_parser.c/.h    # IEEE 802.1AB LLDP frame parser
│   │   ├── cdp_parser.c/.h     # Cisco CDP frame parser
│   │   ├── dhcp_info.c/.h      # DHCP lease information capture
│   │   └── eapol_detect.c/.h   # 802.1X EAPOL detection
│   ├── tests/
│   │   ├── connectivity.c/.h   # Ping, DNS, HTTP connectivity checks
│   │   └── cable_test.c/.h     # PHY register cable quality assessment
│   ├── ui/
│   │   ├── lvgl_ui.c/.h        # LVGL UI (screens, navigation, touch)
│   │   ├── screens.c/.h        # Screen definitions
│   │   ├── tft_init.c/.h       # ST7796S + FT6336U initialization
│   │   └── button.c/.h         # Button input handler with debounce
│   ├── web/
│   │   └── web_server.c/.h     # WiFi AP + captive portal + HTTP results
│   └── storage/
│       ├── sd_card.c/.h        # SD card mount/unmount via SPI
│       └── sd_log.c/.h         # Session logging to SD card
```

### State Machine

```
BOOT → WAIT_FOR_LINK → LINK_UP → (gather LLDP/CDP/DHCP/connectivity) → RESULTS
                ↑                                                          │
                └──────────────── LINK_DOWN ◄──────────────────────────────┘
```

---

## Development Status

### Phase 1: Core Ethernet & LLDP/CDP — ✅ COMPLETE

- [x] ESP-IDF project structure
- [x] Ethernet PHY init (LAN8710A on Olimex)
- [x] Raw frame receive callback
- [x] LLDP frame detection and TLV parser
- [x] CDP frame detection and TLV parser
- [x] PHY register reads for link speed/duplex
- [x] Serial console output
- [x] Test against HP/Aruba switches (verified: HP J9850A / 5406Rzl2)

### Phase 2: DHCP & Connectivity Tests — ✅ COMPLETE

- [x] Inspect-then-forward raw frame path (LLDP/CDP parsed, rest to TCP/IP stack)
- [x] esp_netif with DHCP client
- [x] DHCP lease capture (IP/mask/GW/DNS)
- [x] Gateway ping test
- [x] DNS resolution test (dns.google)
- [x] HTTP connectivity check (clients3.google.com/generate_204)
- [x] Background FreeRTOS task for connectivity tests (SPIRAM stack)
- [x] PoE boot race condition fix (state set before eth_init)

### Phase 3: TFT Display & Touch UI — ✅ COMPLETE

- [x] ST7796S 480x320 TFT via SPI (26 MHz, DMA)
- [x] FT6336U capacitive touch via I2C (polling mode)
- [x] LVGL v9 integration via esp_lvgl_port
- [x] Single scrollable container with all 17 cards (replaced multi-page navigation)
- [x] Boot splash screen
- [x] Waiting/scanning status screens
- [x] Touch scroll navigation
- [x] Port description shown in Port field (e.g., "2 (A2)")
- [x] WiFi AP toggle card (tap to show/hide SSID)

### Phase 4: WiFi AP & Web Interface — ✅ COMPLETE

- [x] SoftAP with auto-generated SSID (LLDPocket-XXXX)
- [x] DNS captive portal on port 53
- [x] HTTP server on port 80 (serves on all interfaces — Ethernet + WiFi)
- [x] HTML results page with LLDP/CDP, DHCP, connectivity, EAPOL, cable data
- [x] Platform text truncated to 79 chars
- [x] HTTP server starts before WiFi AP (Ethernet always reachable)
- [x] WiFi failures are non-fatal (HTTP still works on Ethernet)

### Phase 5: SD Card Logging — ✅ COMPLETE

- [x] SD card mount via SPI (shared bus with TFT)
- [x] Session logging to `/sdcard/lldpkt.log`
- [x] Logs: boot (MAC), link up/down, full network data (LLDP/CDP/DHCP/EAPOL/connectivity/cable)
- [x] POSIX I/O (no stdio — fopen crashes due to internal RAM pressure)
- [x] Buffer allocated from PSRAM

### Phase 6: Additional Protocol Detection — ✅ COMPLETE

- [x] 802.1X EAPOL detection (identifies EAP methods)
- [x] Cable quality assessment via PHY registers (BMSR/ANAR/ANLPAR)
- [x] PoE capability detection from LLDP TLV (Power via MDI)

---

## Remaining Work

### Bugs to Fix

- [ ] **SPI bus contention** — `diskio_sdmmc: sdmmc_read_blocks failed (0x101)` during SD writes (non-fatal, but indicates TFT/SD timing conflicts)
- [ ] **Gateway ping always fails** — `ESP_ERR_NO_MEM` creating ping task (internal RAM exhaustion). Consider removing ping or using a simpler ICMP approach.

### Enhancements

- [ ] **Clean up debug logging** — remove touch_debug_cb and other verbose debug logs from lvgl_ui.c
- [ ] **Multiple result history** — store last N tests on SD card, browse on display
- [ ] **Timestamped logs** — use SNTP to get real time after DHCP (uptime-only currently)
- [ ] **SD card log rotation** — prevent log file from growing unbounded
- [ ] **Web interface auto-refresh** — WebSocket or polling for live updates
- [ ] **Export as JSON** — button on web interface for easy copy/paste

### Hardware / Enclosure

- [ ] Wire LiPo battery with slide switch to Olimex battery input
- [ ] Verify battery charging via PoE
- [ ] Design 3D-printed enclosure (RJ45 on end, TFT window + touch on face)
- [ ] Print, test fit, iterate

### Future (v2)

- [ ] BLE companion app (phone UI, photo tagging, GPS, CSV export)
- [ ] Custom PCB (ESP32-S3 + LAN8720 + PoE + TFT connector — single board)
- [ ] Trunk port VLAN detection (tagged VLANs, not just PVID)
- [ ] Test against Juniper Mist and FortiGate switches

---

## Technical Constraints

### Internal RAM (~18KB free after WiFi + Ethernet + LVGL)
- `fopen()`/`fprintf()` crash — must use POSIX `open()`/`write()`/`close()`
- Static buffers must live in PSRAM (`heap_caps_malloc` with `MALLOC_CAP_SPIRAM`)
- `CONFIG_FATFS_SECTOR_4096` required (512 uses too much RAM)
- `CONFIG_FATFS_LFN_NONE` required (LFN heap allocation crashes WiFi)
- Filenames must be 8.3 format
- Task stacks must use SPIRAM (`xTaskCreateWithCaps` with `MALLOC_CAP_SPIRAM`)

### Flash Size (1.5MB partition, ~2% free)
- Very tight on flash. Adding new features may require optimizing existing code size.
- Consider moving web HTML to SD card if more features needed.

### SPI Bus Sharing
- TFT display and SD card share SPI2_HOST
- SPI clock: 26 MHz for TFT (40 MHz causes queue overflows)
- SD card: 20 MHz
- Occasional `sdmmc_read_blocks failed` during concurrent TFT + SD access (non-fatal)

---

## Key Technical References

- **ESP-IDF Ethernet docs:** https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_eth.html
- **Olimex ESP32-PoE-ISO schematic:** https://github.com/OLIMEX/ESP32-POE-ISO
- **LVGL:** https://lvgl.io — ESP-IDF integration via esp_lvgl_port
- **ST7796 driver:** via esp_lcd component
- **FT6336U touch:** I2C, register-compatible with FT5x06

---

## License

TBD — considering MIT or GPLv3 for open-source release.

**Repository:** `github.com/Joshua-Wise/lldpocket`
