# t-display-progs

Programs for the **LilyGO T-Display-S3** (ESP32-S3, 1.9" 170×320 screen). The suite also runs on the **Freenove 2.8" ESP32 Display** ("CYD", FNK0103B); see [On the CYD](#on-the-freenove-28-cyd).

## TDisplay_Suite: one firmware, all the apps

`TDisplay_Suite/` is the one to flash. What's planned next (network probes, a CDP Viewer, Ethernet) is in [TODO.md](TODO.md). It joins Wi-Fi the easy way and has a menu for picking an app.

| App | What it shows |
| --- | --- |
| **Big Clock** | Just the time: 24-hour HH:MM in seven-segment digits nearly the full height of the screen, with a thin seconds bar. Top key changes colour, bottom key the brightness. |
| **Weather Clock** | Local time and current conditions, 5-day forecast, sunrise/sunset and moon phase, and **weather alerts** from the National Weather Service. While an alert is active the header takes turns showing it, and a warning flashes a red border. Finds its location from your public IP, so it follows the device around (ip-api.com, Open-Meteo, weather.gov; no API keys). Can show a second temperature from a Shelly sensor on your Wi-Fi (e.g. a well house) beside the online one. |
| **XRP Ticker** | XRP's price in dollars, the 24-hour change, high, low and volume, and a price graph over 1 day, 1 week, 1 month or 3 months (top key changes it). Free Coinbase data, no key. |
| **Wi-Fi Meter** | The Wi-Fi it's on, mainly how strong: a needle gauge, dBm, quality, min/max/average and the last few minutes. The top key steps through **Link** (access point, channel and width, 802.11 modes, security and cipher, country, TX power, how long connected), **Network** (IP, mask, gateway, DNS, MAC, hostname, public IP and ISP), **Ping** (latency and loss to the gateway, the DNS server and 1.1.1.1) and **Channels** (how crowded each channel is, and the clearest of 1/6/11). |
| **Wireless Sniffer** | Turns the screen upright (USB at the bottom) for a long list of every **Wi-Fi network** in range, strongest first, with signal, channel and dBm (open ones in yellow, WEP in red, yours highlighted). The top key switches to every **Bluetooth** device advertising nearby, by name or maker. The bottom key pages through the list. It only listens. The ESP32-S3 hears 2.4 GHz Wi-Fi only (no 5 GHz) and Bluetooth Low Energy only (no Classic). |
| **Ham Shack Clock** | Big UTC clock, local time, SFI / SSN / A / K, HF band conditions day and night (hamqsl.com). |
| **LAN Sentinel** | Pings up to 6 hosts in turn and shows up/down and latency, plus a Wi-Fi details page. |
| **GXL Firewall** | The firewall's (pink in / yellow out) and LAN cache's traffic from a [GXL-LANapp](https://github.com/turkey415/GXL-LANapp) Event Server, with a half-hour trace, like the Ops Console banner. |
| **mDNS Browser** | Lists everything announcing itself on the network over multicast DNS (Bonjour): Chromecasts, AirPlay, printers, HomeKit, SSH and web servers, and more, by device name with its type, IP and port. If a full scan hears nothing it says so, which points at multicast being blocked on the Wi-Fi (AP isolation / no IGMP). Listens only. |
| **Ping Graph** | A scrolling latency graph to one target, pinging about twice a second, with the current, average and jitter figures, loss percentage, and a count of spikes over the threshold. The top key steps through the LAN Sentinel targets (gateway first), so the game or GXL server is one tap away. |
| **DHCP Finder** | Broadcasts a DHCP request and lists every server that answers, with its address, MAC, and the address and gateway it offered. One server is normal; a second turns the screen red (a rogue DHCP server, usually someone's home router). It only asks and listens; it never takes an address or changes the device's own networking. |
| **LAN Scanner** | Sweeps the subnet by ARP and lists every device that answers, with its IP, MAC and maker (from the MAC), flagging ones that appear after the first sweep. Local subnet only (/24 around the device's own IP). Sends only ARP requests. |
| **Probe Viewer** | What a [Probe Deck](https://github.com/turkey415/probe-deck) network probe found on the switch port it's plugged into. **Port** page: the switch port in big letters (e.g. `Gi0/7`), VLAN, switch name, model and management IP from CDP/LLDP, link speed, link drops and the seconds from plug-in to an IP. **Tests** page: link, address, gateway ping and loss, DNS, internet and the last speed test, each with a green/red light. **Paths** page: whether the probe's broadcast (UDP 47474) and its web API (found by mDNS) both get through. Many Wi-Fi controllers don't pass broadcasts, so the page says when only one way works. Several probes at once: the bottom key steps through them, and a probe that's just been plugged into a new jack takes the screen by itself. Top key: next page. |
| **Precision Clock** *(CYD only)* | A full-screen lab clock in LED digits to the millisecond (tap them: red, green or dark blue), kept by its own NTP client: six servers (Cloudflare, Google, Apple, NIST, pool.ntp.org, the gateway) polled every 64 s, the shortest-round-trip sample per server, Marzullo's algorithm to drop servers that disagree, and a weighted average of the rest. It learns the board crystal's drift (ppm and s/day) from 30 minutes of offsets so the time runs smoothly between samples, and shows the honest error bound (±) NTP computes. Also UTC, Unix time, MJD, day of year and ISO week. Tap: the clock → a source table like `ntpq -p` (reach, offset, delay, jitter, which server is in use or rejected) → three clocks side by side (NTP, the ESP32's own SNTP clock, and the bare crystal left free-running since the first sync) with a graph of the crystal's wander. BOOT: sync again now. |

### Wi-Fi and settings

At power-on it tries the last network set up from a phone, then the home network in `secrets.h` if you made one. If neither is in range it opens a hotspot called **TDisplay-Setup**:

1. Join `TDisplay-Setup` from a phone. A setup page opens (or browse to `192.168.4.1`).
2. **Configure WiFi**: pick the network, type its password, and fill in the settings below it.
   **Setup** changes only the settings.
3. Save. The device restarts and joins.

Settings on that page:

- **GXL Event Server**: e.g. `http://10.0.0.5:8000`, and the event's **agent key** (the one the PC agents use). Needed for GXL Firewall.
- **Callsign** and **local time zone** (POSIX, e.g. `CST6CDT,M3.2.0,M11.1.0`) for the Ham Shack Clock.
- **LAN Sentinel hosts**: `Label=target,Label=target`, up to 6. A target is an IP, a hostname, or `GW` for the gateway. Empty means the gateway, Cloudflare (1.1.1.1), Control D (76.76.2.0), Quad9 (9.9.9.9), OpenDNS (208.67.222.222) and google.com.
- **Shelly temp sensor IP** (optional): a Shelly device on the Wi-Fi with a temperature sensor, e.g. `10.2.1.60`. The Weather Clock then shows that local reading (say, a well-house sensor) next to the online temperature, labelled **AIR** and **WELL**. Blank turns it off. Uses the Shelly local HTTP API (Gen1 or Gen2+), which is on with no login unless you set a device password.
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
| Menu → **Power off** | Turn it off (deep sleep). Press the **top** key (or RST) to turn it back on. |

### Battery

With a LiPo plugged into the battery connector, every screen shows the charge in the top-right corner (Big Clock: bottom-right): a percentage and a battery that empties, green, then yellow under 50%, red under 20%. On USB it shows a charging bolt and `USB`. The percentage comes from the cell's voltage, so it's a guide, and it reads a little low while Wi-Fi is busy.

The board has no power switch: the battery is wired straight to the charger. **Power off** puts it in deep sleep (screen, backlight and Wi-Fi off), which still draws a little, so for weeks on a shelf, unplug the battery. On battery it also turns itself off when the cell is flat, so it doesn't keep browning out and restarting.

The menu also has **Check for update** and **Wi-Fi & settings**.

### On the Freenove 2.8" CYD

The same firmware, built with `tools/build.sh cyd` (FNK0103B: classic ESP32, 240×320 ST7789 TN, resistive touch, 4 MB flash, no PSRAM). The apps keep their 320×170 layout at the top of the screen and the spare 70 pixels show a **status bar**: clock and date, Wi-Fi signal, network name and IP, battery or USB. Portrait apps put it in a column on the right.

The CYD's only readable button is BOOT (the other is RESET), so the touchscreen stands in for the top key:

| Do this | Same as on the T-Display |
| --- | --- |
| Tap anywhere on the screen | Press top |
| Hold the screen | Hold top (Wi-Fi & settings hotspot, also at power-on) |
| Press BOOT | Press bottom |
| Hold BOOT | Hold bottom (app menu). In the menu you can also tap a row to open it. |
| Tap while powered off | Turns it back on |

In the **mDNS Browser** (portrait), tap the status column and it slides off to the right so the list widens to 240 px; tap the right edge to bring it back. The CYD also has the **Precision Clock**, which uses the whole screen.

Differences: the setup hotspot is **CYD-Setup**, the Wireless Sniffer has no Bluetooth view (Bluetooth doesn't fit beside Wi-Fi on the classic ESP32), and it updates from `CYD_Suite.bin` in the same releases.

## Updates over Wi-Fi

Devices update themselves from this repo's [releases](https://github.com/turkey415/t-display-progs/releases). At power-on and once a day each one reads `version.txt` from the latest release and, if it is newer than what it runs, downloads `TDisplay_Suite.bin`, installs it and restarts. You see a progress bar while it does. Settings and Wi-Fi are kept. **Check for update** in the menu does the same at any time. Turn the automatic check off on the setup page if you'd rather update by hand.

To publish a new version:

1. Change `FW_VERSION` in `TDisplay_Suite/version.h` (e.g. `1.2.0`) and merge.
2. Push the tag on main: `git tag v1.2.0 && git push origin v1.2.0`.
3. The **Build** workflow compiles it and makes the release (`TDisplay_Suite.bin`, `TDisplay_Suite-full.bin`, `CYD_Suite.bin`, `CYD_Suite-full.bin`, `version.txt`). It refuses if the tag and `version.h` differ.

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

Or `tools/build.sh s3` / `tools/build.sh cyd` (add a port to flash, `-` to only build). The CYD build sets TFT_eSPI up from `tools/cyd_flags.txt`, so `User_Setup_Select.h` can stay on the T-Display setup.

On Windows the port is a `COM` number. If upload won't start, hold BOOT, tap RST, let go of BOOT, and try again.

## Adding an app

Write `app_<name>.h` in `TDisplay_Suite/` with a namespace holding `start()`, `run()` and `press(int pin)` (call `setPortrait(true)` in `start()` for an upright screen, as `app_sniffer.h` does) (see `app_ham.h` for a small one), draw into `spr` and call `spr.pushSprite(0, 0)`, then include it and add a row to `apps[]` in `TDisplay_Suite.ino`. Wi-Fi, settings, the menu and holding the keys are taken care of already.
