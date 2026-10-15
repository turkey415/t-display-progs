# t-display-progs

Programs for the **LilyGO T-Display-S3** (ESP32-S3, 1.9" 170×320 screen).

## TDisplay_Suite: one firmware, all the apps

`TDisplay_Suite/` is the one to flash. It joins Wi-Fi the easy way and has a menu for picking an app.

| App | What it shows |
| --- | --- |
| **Weather Clock** | Local time, current conditions, 5-day forecast. Finds its location from your public IP, so it follows the device around (ip-api.com + Open-Meteo, no API keys). |
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
- **Units** F or C, **clock** 24 or 12.

If Wi-Fi is lost for 2 minutes the device restarts and goes through the steps again, so moving it somewhere new just works.

### Keys

The top key is GPIO14, the bottom one is BOOT (GPIO0).

| Do this | To |
| --- | --- |
| Hold bottom | Open the app menu: top moves down, bottom picks. The app picked starts at every power-on. |
| Hold top | Open the Wi-Fi & settings hotspot |
| Hold top while powering on | Same, before trying any network |
| Press top | Next page (Weather, Ham, Sentinel) or firewall ↔ LAN cache (GXL) |
| Press bottom | Refresh now |

### GXL Firewall

It reads `GET /agent/gear` on the Event Server with the event's agent key, every 10 seconds; the server reads the switches every 20. Needs **GXL server 0.6.10 or newer**. Mark the firewall and LAN cache ports in the Ops Console (Switches › Ports) first, or the screen says nothing is marked.

### Build defaults (optional)

Copy `TDisplay_Suite/secrets.h.example` to `TDisplay_Suite/secrets.h` to build in your home Wi-Fi, GXL server, agent key, callsign and the first app. `secrets.h` is in `.gitignore`. Anything set from the phone takes over from it.

## Standalone sketches

`standalone/` has the first single-purpose versions: Weather Clock (with the setup hotspot), Ham Shack Clock and LAN Sentinel. The last two need your Wi-Fi typed into the sketch (`your-wifi` / `your-password`). The suite does everything they do.

## Building

With [arduino-cli](https://arduino.github.io/arduino-cli/), ESP32 core 3.x and these libraries:

- **TFT_eSPI**, with `User_Setup_Select.h` edited to include `Setup206_LilyGo_T_Display_S3.h` (and nothing else)
- **ArduinoJson** 7
- **WiFiManager** (tzapu)
- **ESP32Ping** (marian-craciunescu)

```sh
arduino-cli compile --upload -p /dev/ttyACM0 \
  --fqbn esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB \
  TDisplay_Suite
```

On Windows the port is a `COM` number. If upload won't start, hold BOOT, tap RST, let go of BOOT, and try again.

## Adding an app

Write `app_<name>.h` in `TDisplay_Suite/` with a namespace holding `start()`, `run()` and `press(int pin)` (see `app_ham.h` for a small one), draw into `spr` and call `spr.pushSprite(0, 0)`, then include it and add a row to `apps[]` in `TDisplay_Suite.ino`. Wi-Fi, settings, the menu and holding the keys are taken care of already.
