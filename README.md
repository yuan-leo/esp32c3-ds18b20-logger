# ESP32-C3 DS18B20 temperature logger

Local-only ESP32-C3 temperature logger with a responsive Wi-Fi dashboard, interactive graph, CSV export, and persistent flash log. One reading every **5 seconds**. Designed for ESP32-C3 boards with **4 MB flash**.

## Wiring

| DS18B20 | ESP32-C3 |
| --- | --- |
| VDD | 3V3 |
| GND | GND |
| DATA | GPIO4 |
| 4.7 kΩ resistor | Between GPIO4 and 3V3 |

Use a three-wire DS18B20, not parasite mode. Check waterproof probe wire colors against the manufacturer's documentation. **Never connect 5 V to ESP32-C3 GPIO.**

## Flash using Android

1. Open this repository's **Actions** tab and select **Compile ESP32-C3 factory firmware**. The workflow automatically runs on pushes to `main`, or use **Run workflow**.
2. After a successful build, download the artifact named `temperature-logger-esp32c3`.
3. Extract `temp-logger-esp32c3-factory-0x0.bin` from the artifact onto your Android device.
4. Use an open-source Android flasher such as [Termux-ESP-Flasher / nrflash](https://github.com/7wp81x/Termux-ESP-Flasher). Flash the **factory image** at **address 0x0**, not the standalone `firmware.bin`.

FOSS flasher setup in Termux (install Termux and Termux:API from F-Droid):

```sh
pkg update
```

```sh
pkg install python termux-api libusb
```

```sh
pip install nrflash
```

```sh
termux-setup-storage
```

Connect ESP32-C3 over USB data + OTG, grant USB permission, and identify it:

```sh
nrflash probe
```

Flash the factory image:

```sh
nrflash write --chip esp32c3 --offset 0x0 ~/storage/downloads/temp-logger-esp32c3-factory-0x0.bin --verify
```

If required, hold BOOT, press RESET, release BOOT, then retry.

## First use

The ESP32 creates a Wi-Fi network `TempLogger-XXXXXX`, password **`TempLog1234`**. Connect and open **http://192.168.4.1/**. You can join an existing 2.4-GHz network from the dashboard. The device hotspot stays available.

The browser attempts to synchronize the clock from the phone. With no Wi-Fi/NTP/browser sync, the ESP32 only knows uptime and cannot reconstruct real dates after power loss.

## Data and retention

- Sample interval: 5 seconds; DS18B20 at 12-bit resolution.
- Rolling log: 12 × 96 KiB binary segments. Approximately 147,456 readings / 8.5 days.
- Writes batched approximately every 60 seconds to reduce flash wear; a sudden power loss can lose recent buffered readings.
- `/export.csv` downloads complete stored samples, including error readings.
- `/api/history?samples=720` provides the downsampled graph and summary statistics.
- Storage uses LittleFS in the 4-MB custom partition table.
- No required cloud service, app, or Internet connection for routine logging.

## Build locally

```sh
python -m pip install platformio==6.2.0 esptool==4.11.0
```

```sh
pio run -e esp32c3
```

```sh
python scripts/make_factory.py
```

This creates `dist/temp-logger-esp32c3-factory-0x0.bin` and SHA-256 checksums.

## Security and limitations

The Wi-Fi hotspot uses the default password `TempLog1234`. Change the `AP_PASSWORD` constant in `src/main.cpp` and rebuild for a private deployment. The local web API uses **HTTP and has no authentication**. Keep this board on a trusted LAN, and never port-forward to it. Clearing logs is irreversible. Avoid full-chip erase if you need to retain historical readings.

Firmware runs on a standard 4-MB ESP32-C3 target; board-specific boot and flash requirements may differ. GitHub Actions must complete successfully before its binary is considered build-verified.

## License

GNU General Public License v3.0, matching this repository's existing LICENSE. Third-party Arduino-ESP32, OneWire, and DallasTemperature libraries retain their own licenses.
