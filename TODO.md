# Coming soon

Plans and ideas that aren't built yet. When one ships, move it into the README and delete it here.

## Network probes + CDP Viewer

**The idea:** small **probe** boards plug into switch ports (powered by PoE), test the port, and send what they find to a T-Display running a new **CDP Viewer** app. The probe does the network work and the T-Display just shows the results. You can carry the T-Display while the probe sits in the jack, or leave probes in several ports and flip between them.

### First probe: ESP32-S3 Ethernet board with PoE

The Waveshare-style **ESP32-S3 Ethernet Dev Board with PoE module** (Amazon B0FG7V5HZP): ESP32-S3 + W5500 Ethernet (10/100) + a PoE add-on board, TF card slot, camera connector, and no screen. Same chip as the T-Display, so the Arduino setup and much of the code carry over.

- Firmware goes in its own folder here, e.g. `Probe_ESP32/`, with the same Wi-Fi setup-portal base and GitHub-release updates as TDisplay_Suite.
- To check once it arrives: the exact W5500 pins (SPI, CS, INT, RST), whether there's a status LED or button to use, and that it boots on PoE alone.
- **Limit:** the W5500 is 10/100 only, so it reports 100 Mbps even on a gigabit port. See [Faster probes](#faster-probes-gigabit-and-sfp).

### What a probe tests

| Test | How |
| --- | --- |
| **CDP** | Listen for Cisco Discovery Protocol frames (every 60 s by default, multicast `01:00:0c:cc:cc:cc`): switch name, port (e.g. `Gi0/14`), native VLAN, voice VLAN, platform/model, IOS version, management IP, duplex. Needs the W5500 in MACRAW mode to see raw frames. |
| **LLDP** | The same idea for non-Cisco switches (multicast `01:80:c2:00:00:0e`). |
| **Link** | Up/down, speed, duplex, how long it took to come up, link flaps. |
| **PoE** | On with no USB = the port gives PoE. (The class the switch granted shows in CDP on Cisco.) |
| **DHCP** | Got an address? IP, mask, gateway, DNS, lease time, DHCP server, and how long it took. |
| **Gateway** | Ping it: latency and loss. |
| **DNS** | Look up a name through the DHCP-given server. |
| **Internet** | Reach a known address (e.g. 1.1.1.1) and an HTTP check. |
| **Custom** | Ping a list of targets, like LAN Sentinel (game server, GXL Event Server). |

Tests run on link-up and then repeat (e.g. every 30 s), so unplugging the probe and moving it to a new jack gets fresh results without pressing anything.

### How results get to the T-Display

A probe sends every result over **every path it can**, and the viewer shows which paths delivered it. Which paths arrive is a test in itself.

| Path | Notes |
| --- | --- |
| **ESP-NOW** (direct radio) | The main path. ESP32-to-ESP32, no Wi-Fi network or pairing needed, works anywhere, ~100 m+ line of sight. Messages max 250 bytes (1470 with ESP-NOW v2 on newer ESP-IDF), so keep them compact or split them. Both ends must be on the same Wi-Fi channel: the probe follows the T-Display's channel, or sends on all of 1/6/11. |
| **UDP over the wired network** | The probe broadcasts from its Ethernet port and the T-Display listens on Wi-Fi (a fixed port, e.g. 47474). If this one arrives, the port reaches the T-Display's network, which proves VLAN/routing. Directed to the T-Display's IP if broadcasts don't cross VLANs. |
| **Bluetooth LE** | Also needs no network. Add last, since ESP-NOW covers the same case with less code. Handy if a phone page or app ever reads probes too. |

**Message:** one small JSON (or packed binary for ESP-NOW) per update: probe ID, probe name, sequence number, uptime, then the results above. The same format over every path, so the viewer doesn't care how it arrived, and so non-ESP32 probes (below) can send it too.

### Several probes at once

There could be several probes out at any time, so the viewer must handle more than one:

- **Probe list** screen: every probe heard, with name, switch port (from CDP), a status dot (all tests pass / some fail / gone quiet) and last heard ("12 s ago"). Top key moves, bottom key opens one.
- **Probe detail** screen: one probe's results, paged like the Wi-Fi Meter (CDP → link & PoE → DHCP → ping/DNS/internet → paths heard).
- **Names:** a probe starts as its MAC's last 4 digits (e.g. `Probe-3F2A`). Rename it on the probe's setup portal (e.g. "Stage left", "Admin table"), and the name travels in its messages.
- **Gone quiet:** grey after ~2 missed updates, dropped from the list after e.g. 10 minutes (or kept, marked "last seen").
- **Follow mode:** optionally jump to whichever probe just changed (e.g. was just plugged into a new jack), so walking a probe around the room needs no button presses.
- **Remember the last one picked**, like the app menu does.
- Maybe later: a **probe ID/key** setting so only your probes show up at a LAN party full of other people's gear.

### CDP Viewer screen (rough idea)

```
 CDP Viewer          Probe-3F2A   87% ⚡
 SW-CORE-3560        Gi0/14
 VLAN 20  voice 100  100 Full  PoE ✓
 IP 10.2.20.57   GW ✓ 2 ms   DNS ✓
 Internet ✓          via radio + network
```

## Faster probes: gigabit and SFP

**No ESP32 does gigabit.** The W5500 and the ESP32's built-in Ethernet are 10/100, and the newer ESP32-P4 is still 100 Mbps. A gigabit or faster probe means a small Linux box speaking the **same probe message** over UDP (no ESP-NOW, so it uses the wired-network path).

| Option | Speed | Notes |
| --- | --- | --- |
| **GL.iNet Slate 7** (already owned) | 2.5 GbE WAN | Already runs the PorkChopCheck pocket tester (`router/` in PorkChopCheck), which measured ~904/920 Mbps with iperf3. Adding a "send probe results" step makes it the first gigabit probe, with its own screen too. Needs an LLDP/CDP listener added (tcpdump is there). |
| **Banana Pi BPI-R4** | 2× 10G SFP+ | OpenWrt router board. A 10G **SFP+** probe for fiber/uplink ports. Check current models and prices before buying. |
| **Banana Pi BPI-R3** | 2× 2.5G SFP | Cheaper SFP option. |
| **Raspberry Pi 4/5** | 1 GbE (+ USB 2.5G adapter) | Easy to code for, PoE HAT available. |

Plan: get the ESP32 probe working first (cheap, many of them), then teach the Slate 7 to send the same messages, then decide on SFP+ if fiber ports need testing.

## Ethernet on the T-Display itself

A **HiLetgo W5500 module** (Amazon B08KXM8TKJ) wired to the T-Display's header pins with ~7 female-female jumpers (3V3, GND, MOSI, MISO, SCK, CS, optional INT/RST). Pin chart to be written when it arrives (headers may need soldering first).

- Lets a single T-Display run the probe tests itself, in one handheld box.
- **PoE for it:** a PoE splitter with a USB-C 5 V output. One cable from the switch: data goes to the W5500, power to the T-Display's USB-C (and charges the battery).
- Also possible: a "use Ethernet if connected" option for the other apps (Sentinel, GXL Firewall).

## Smaller things

- **Battery bolt (v1.3.1):** tune the charger-detection numbers (`STEP_UP_V`, `STEP_DOWN_V`, `TREND_V` in `common.h`) against real serial logs on a power bank and a wall charger.
