# PortLens

A pocket Ethernet tester you build yourself. Plug it into a wall jack and it shows which switch and port you're on, the VLAN, PoE, the DHCP lease, and whether the network works.

It's an [Olimex ESP32-PoE-ISO](https://www.olimex.com/Products/IoT/ESP32/ESP32-POE-ISO/open-source-hardware) and a 1.44" display. On a PoE port it runs from the cable.

**Website:** [portlens.dev](https://portlens.dev)

## What it shows

| | Source |
|---|---|
| Switch name, port, port description, model, management IP | LLDP or CDP |
| VLAN ID and name | LLDP (port VLAN) or CDP (native VLAN) |
| PoE | LLDP (supported / enabled) or CDP (watts available) |
| IP address, netmask, gateway, DNS | DHCP |
| Gateway ping, DNS lookup, HTTP request | Run once after each DHCP lease |
| 802.1X | Detected from EAPOL frames |
| Link speed and duplex | Ethernet PHY |

The display alternates between two pages every 10 seconds. The full record is on a web page: join the open Wi-Fi network `PortLens-XXXX` and it opens as a captive portal (or browse to `192.168.4.1`). The page also has a ping tool, and the same data is available as JSON at `/api/results`.

## Parts

| Part | Notes |
|---|---|
| [Olimex ESP32-PoE-ISO](https://www.olimex.com/Products/IoT/ESP32/ESP32-POE-ISO/open-source-hardware) | WROOM and WROVER versions both work. [Schematics](https://github.com/OLIMEX/ESP32-POE-ISO). |
| 1.44" ST7735 SPI display, 128×128 | Generic 8-pin module. Search [Amazon](https://www.amazon.com/s?k=1.44+inch+TFT+ST7735+128x128+SPI) or [AliExpress](https://www.aliexpress.com/w/wholesale-1.44-tft-st7735-128x128-spi.html). |
| 8 jumper wires, USB cable | USB is only needed for flashing. |
| 3D-printed enclosure | Optional. STEP file in [`enclosure/`](enclosure/). |

## Wiring

| Display pin | Board |
|---|---|
| VCC | 3.3V |
| GND | GND |
| CS | GPIO 5 |
| RESET | GPIO 15 |
| A0 / DC | GPIO 2 |
| SDA / MOSI | GPIO 14 |
| SCK | GPIO 33 |
| LED | 3.3V |

Pins are defined in [`main/config.h`](main/config.h).

## Flash it

**Prebuilt firmware.** No toolchain needed, only Python:

```sh
git clone https://github.com/Joshua-Wise/portlens.git
cd portlens
pip install esptool pyserial
python tools/flash_station.py
```

Plug a board in over USB and it gets flashed with the binaries in `docs/firmware/`. Keep plugging boards in to do a batch; each one's Ethernet MAC is logged to `flash_log.csv`. If the board doesn't appear as a serial port, install the [CH340 driver](https://www.wch-ic.com/downloads/CH341SER_ZIP.html).

**From source.** Install [ESP-IDF 5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32/get-started/index.html) (tested with 5.5.2), then:

```sh
git clone https://github.com/Joshua-Wise/portlens.git
cd portlens
idf.py build
idf.py -p PORT flash monitor
```

Or build in Docker without installing anything:

```sh
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5.2 \
  bash -c 'pip install -q "idf-component-manager~=2.4.11" && idf.py build'
```

> If the build prints LVGL deprecation warnings, your IDF component manager is older than 2.4.11 and has fetched a newer LVGL than the one pinned in `dependencies.lock`. Run `pip install -U "idf-component-manager~=2.4.11"` in the ESP-IDF Python environment, delete `managed_components/`, and build again.

## Configuration

Everything is in [`main/config.h`](main/config.h):

| Flag | Default | |
|---|---|---|
| `FEATURE_DISPLAY` | 1 | The TFT. Set to 0 to run headless (serial log and web page only). |
| `FEATURE_WIFI_AP` | 1 | The Wi-Fi access point and captive portal. |
| `FEATURE_DEMO_MODE` | 0 | Skips Ethernet and shows fake data, for working on the UI. |

## Security

The Wi-Fi network is open. While the device is plugged in, anyone in range can read the results and use the ping tool, which sends pings into the wired network. The web page is also reachable from the wired side. Use PortLens on networks you're allowed to test, or set `FEATURE_WIFI_AP` to 0.

On each DHCP lease the device pings the gateway, resolves `dns.google`, and requests `http://clients3.google.com/generate_204`. LLDP, CDP and EAPOL are received only.

## Limits

- The PHY is 10/100, so gigabit ports link at 100 Mbps.
- No cable test (wiremap, length).
- Only the untagged VLAN is reported.
- Developed against HPE Aruba switches. Other LLDP and CDP gear should work; please open an issue if it doesn't.

## Repository layout

```
main/        firmware (ESP-IDF, C)
  eth/         Ethernet init and raw frame capture
  protocols/   LLDP, CDP, DHCP and EAPOL
  tests/       connectivity tests
  ui/          display (LVGL)
  web/         Wi-Fi AP, captive portal, HTTP API
docs/        released firmware binaries, plus a browser flasher page (not hosted)
tools/       flashing helpers
enclosure/   3D-printable case
```

## Tools

- `tools/flash_station.py` flashes every board you plug in, several at once. It uses your own `build/` if there is one, otherwise `docs/firmware/`; force either with `--source build` or `--source docs`. Run it with `--help` for the rest. `tools/flash-station.sh` runs it inside the ESP-IDF Python environment.
- `tools/update-flasher.sh` copies a fresh build into `docs/firmware/`.

## License

[MIT](LICENSE)
