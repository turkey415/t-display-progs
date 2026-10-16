# t-display-progs

Programs for the **LilyGO T-Display-S3** (ESP32-S3, 1.9" 170×320 screen).

## TDisplay_Suite: one firmware, all the apps

`TDisplay_Suite/` is the one to flash. It joins Wi-Fi the easy way and has a menu for picking an app.

| App | What it shows |
| --- | --- |
| **Big Clock** | Just the time: 24-hour HH:MM in seven-segment digits nearly the full height of the screen, with a thin seconds bar. Top key changes colour, bottom key the brightness. |
| **Weather Clock** | Local time and current conditions, 5-day forecast, sunrise/sunset and moon phase, and **weather alerts** from the National Weather Service. While an alert is active the header takes turns showing it, and a warning flashes a red border. Finds its location from your public IP, so it follows the device around (ip-api.com, Open-Meteo, weather.gov; no API keys). |
| **XRP Ticker** | XRP's price in dollars, the 24-hour change, high, low and volume, and a price graph over 1 day, 1 week, 1 month or 3 months (top key changes it). Free Coinbase data, no key. |
| **Wi-Fi Meter** | The Wi-Fi it's on, mainly how strong: a needle gauge, dBm, quality, min/max/average and the last few minutes. The top key steps through **Link** (access point, channel and width, 802.11 modes, security and cipher, country, TX power, how long connected), **Network** (IP, mask, gateway, DNS, MAC, hostname, public IP and ISP), **Ping** (latency and loss to the gateway, the DNS server and 1.1.1.1) and **Channels** (how crowded each channel is, and the clearest of 1/6/11). |
| **Wireless Sniffer** | Turns the screen upright (USB at the bottom) for a long list of every **Wi-Fi network** in range, strongest first, with signal, channel and dBm (open ones in yellow, WEP in red, yours highlighted). The top key switches to every **Bluetooth** device advertising nearby, by name or maker. The bottom key pages through the list. It only listens. The ESP32-S3 hears 2.4 GHz Wi-Fi only (no 5 GHz) and Bluetooth Low Energy only (no Classic). |
| **Ham Shack Clock** | Big UTC clock, local time, SFI / SSN / A / K, HF band conditions day and night (hamqsl.com). |
| **LAN Sentinel** | Pings up to 6 hosts in turn and shows up/down and latency, plus a Wi-Fi details page. |
| **GXL Firewall** | The firewall's (pink in / yellow out) and LAN cache's traffic from a [GXL-LANapp](https://github.com/turkey415/GXL-LANapp) Event Server, with a half-hour trace, like the Ops Console banner. |

### Wi-Fi and settings

At power-on it tries the last network set up from a phone, then the home network in `secrets.h` if you made one. If neither is in range it opens a hotspot called **TDisplay-Setup**:

1. Join `TDisplay-Setup` from a phone. A setup page opens (or browse to `192.168.4.1`).
2. **Configure WiFi**: pick the network, type its password, and fill in the settings below it.
   **Setup** changes only the settings.
3. Save. The device restarts and joins.

Settings on that page:

- **GXL Event Server**: e.g. `http://10.0.0.5:8000`, and the event's **agent key** (the one the PC agents use). Needed for GXL Firewall.
- **Callsign** and **local time zone** (POSIX, e.g. `CST6CDT,M3.2.0,M11.1.0`) for the Ham Shack Clock.
- **LAN Sentinel hosts**: `Label=target,Label=target`, up to 6. A target is an IP, a hostname, or `GW` for the gateway. Empty means gateway, 1.1.1.1, 8.8.8.8 and google.com.
- **Units** F or C, **clock** 24 or 12 (for the Weather and Ham clocks; the Big Clock is always 24-hour).
- **Install updates by itself**: yes or no (see below).

If Wi-Fi is lost for 2 minutes the device restarts and goes through the steps again, so moving it somewhere new just works.

### Keys

The top key is GPIO14, the bottom one is BOOT (GPIO0).

| Do this | To |
| --- | --- |
| Hold bottom | Open the app menu: top moves down, bottom picks. The app picked starts at every power-on. |
| Hold top | Open the Wi-Fi & settings hotspot |
| Hold top while powering on | Same, before trying any network |
| Press top | Next page (Weather, Wi-Fi Meter, Ham, Sentinel), next graph range (XRP), Wi-Fi ↔ Bluetooth (Sniffer), firewall ↔ LAN cache (GXL), next colour (Big Clock) |
| Press bottom | Refresh now (Big Clock: brightness, which every app keeps; Sniffer: next page of the list; Wi-Fi Meter: reset the meter or ping stats) |

The menu also has **Check for update** and **Wi-Fi & settings**.

## Updates over Wi-Fi

Devices update themselves from this repo's [releases](https://github.com/turkey415/t-display-progs/releases). At power-on and once a day each one reads `version.txt` from the latest release and, if it is newer than what it runs, downloads `TDisplay_Suite.bin`, installs it and restarts. You see a progress bar while it does. Settings and Wi-Fi are kept. **Check for update** in the menu does the same at any time. Turn the automatic check off on the setup page if you'd rather update by hand.

To publish a new version:

1. Change `FW_VERSION` in `TDisplay_Suite/version.h` (e.g. `1.2.0`) and merge.
2. Push the tag on main: `git tag v1.2.0 && git push origin v1.2.0`.
3. The **Build** workflow compiles it and makes the release (`TDisplay_Suite.bin`, `TDisplay_Suite-full.bin`, `version.txt`). It refuses if the tag and `version.h` differ.

Every pull request gets a build too, with the files attached to the run.

A .bin can also be uploaded by hand from the setup hotspot's **Update** page, and `TDisplay_Suite-full.bin` is for a first flash over USB (write it at offset 0x0 with esptool or a web flasher).

The download uses HTTPS but does not check GitHub's certificate. The device checks that the file is a whole, valid ESP32 firmware image before switching to it, but not who built it, so someone able to tamper with your network could in principle give it other firmware. That's a fair trade for a desk display; say so if you want signed updates.

### GXL Firewall

It reads `GET /agent/gear` on the Event Server with the event's agent key, every 10 seconds; the server reads the switches every 20. Needs **GXL server 0.6.10 or newer**. Mark the firewall and LAN cache ports in the Ops Console (Switches › Ports) first, or the screen says nothing is marked.

### Build defaults (optional)

Copy `TDisplay_Suite/secrets.h.example` to `TDisplay_Suite/secrets.h` to build in your home Wi-Fi, GXL server, agent key, callsign and the first app. `secrets.h` is in `.gitignore`. On first boot these are saved to the device, so the releases from GitHub (built without your `secrets.h`) keep them. Anything set from the phone takes over.

## Standalone sketches

`standalone/` has the first single-purpose versions: Weather Clock (with the setup hotspot), Ham Shack Clock and LAN Sentinel. The last two need your Wi-Fi typed into the sketch (`your-wifi` / `your-password`). The suite does everything they do.

## Building

With [arduino-cli](https://arduino.github.io/arduino-cli/), ESP32 core 3.x and these libraries:

- **TFT_eSPI** 2.5.43, with `User_Setup_Select.h` edited to include `Setup206_LilyGo_T_Display_S3.h` (and nothing else)
- **ArduinoJson** 7.4
- **WiFiManager** 2.0.17 (tzapu)
- **ESP32Ping** (marian-craciunescu)

```sh
arduino-cli compile --upload -p /dev/ttyACM0 \
  --fqbn esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB \
  TDisplay_Suite
```

On Windows the port is a `COM` number. If upload won't start, hold BOOT, tap RST, let go of BOOT, and try again.

## Adding an app

Write `app_<name>.h` in `TDisplay_Suite/` with a namespace holding `start()`, `run()` and `press(int pin)` (call `setPortrait(true)` in `start()` for an upright screen, as `app_sniffer.h` does) (see `app_ham.h` for a small one), draw into `spr` and call `spr.pushSprite(0, 0)`, then include it and add a row to `apps[]` in `TDisplay_Suite.ino`. Wi-Fi, settings, the menu and holding the keys are taken care of already.
