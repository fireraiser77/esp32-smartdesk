# ESP32 Smart Desk

An ESP32 running [ESPHome](https://esphome.io) that stands in for the handset of a
cheap sit/stand desk, so Home Assistant can raise and lower it, read its height,
and stand me up when I've been sitting too long.

- No soldering, no extra parts: an ESP32 dev board and a spare Ethernet cable.
- The original handset is untouched. Unplug the ESP32, plug the handset back in.
- Go-to-height instead of the box's presets: set Sit and Stand heights in inches.
- Auto-stand in Home Assistant driven by an mmWave presence sensor.

Write-up with photos and the story behind it:
[benmarflitt.com/projects/esp32-smart-desk](https://benmarflitt.com/projects/esp32-smart-desk)

The bus protocol came from
[dong4j/desk-gateway](https://github.com/dong4j/desk-gateway), which reverse-engineered
the same handset. This project wouldn't exist without it.

## Will this work on my desk?

It's written for this combination:

| Part | Marking |
|---|---|
| Control box | **MXKZH06** |
| Handset | **MXSK05_A** (V1.4 here): Up, Down, 1–4, 3-digit LED display |
| Handset chip | **TM1650** |

Lots of no-name desks use these. Open your handset (a few screws) and look for the
board marking and a 16-pin TM1650. If you have a different handset chip, the
approach still applies but the addresses and key codes won't; see
[docs/protocol.md](docs/protocol.md) for what to look for with a logic analyzer.

## What you need

- An ESP32 dev board (original ESP32 / ESP-WROOM-32; I used a NodeMCU-32S). A
  screw-terminal breakout board makes the no-solder wiring easy.
- An Ethernet patch cable you're willing to cut. The handset port on the control
  box takes a standard RJ45 plug.
- A USB power supply for the ESP32.
- Home Assistant with the ESPHome add-on (or the ESPHome CLI).
- Optional: a presence sensor for auto-stand.

## Wiring

Cut the Ethernet cable, keep the end with the plug, and strip the wires for pins
2, 3 and 4. With a T568B cable those are:

| RJ45 pin | Signal | T568B colour | ESP32 |
|---|---|---|---|
| 1 | 3.3 V from the box | orange/white | **leave unconnected** |
| 2 | CLK | orange | GPIO18 |
| 3 | GND | green/white | GND |
| 4 | DAT | blue | GPIO19 |

Check your cable's colours against the pin order rather than trusting the table.
The handset's own cable uses red, white, green, black for pins 1–4.

Power the ESP32 from USB, not from pin 1. Unplug the handset from the control box
and plug the cut cable in its place.

> **Never send Up and Down together.** On this box that's the factory reset combo.
> The firmware refuses to, but keep it in mind if you change it.

## Install

1. In the ESPHome dashboard, create a new device (or use the CLI). Copy
   [`firmware/smart-desk.yaml`](firmware/smart-desk.yaml) over its config and put
   [`firmware/desk_panel.h`](firmware/desk_panel.h) in the same folder
   (`/config/esphome/` on the Home Assistant add-on).
2. Copy [`firmware/secrets.yaml.example`](firmware/secrets.yaml.example) to
   `secrets.yaml` in that folder (or add the keys to your existing one) and fill
   in your own Wi-Fi details, a new API encryption key and passwords.
3. If your board isn't a NodeMCU-32S, change `board:` under `esp32:`.
4. Install over USB the first time, wirelessly after that.
5. Add the device in Home Assistant (Settings → Devices & services → ESPHome).

### Check it's working

Open the device page in Home Assistant, or browse to the ESP32's IP for the
built-in web page.

- **Control box connected** should be **on**. If it's off, check GND and CLK.
- **Bus raw** shows the display bytes and the key being sent, e.g.
  `00 00 00 D3 key 2E`. The counters at the end should keep climbing.
- Press **Nudge Up**. The first press may only wake the box (display comes on);
  the second moves the desk and **Height** should update.

## Using it

| Entity | What it does |
|---|---|
| **Sit / Stand** | Drive to the Sit Height / Stand Height and stop there |
| **Stop** | Let go immediately; the desk stops |
| **Nudge Up / Down** | Move for one second |
| **Height** | Inches, decoded from the display data |
| **Position** | `sitting` / `standing`, split halfway between the two heights |
| **Sitting Minutes** | Minutes since the desk last went to sitting height |
| **Sit Height / Stand Height** | Settings (22–52 in). Adjust to suit you |
| **Stop Early By** | How far before the target to let go (default 0.3 in) |

### Tuning Stop Early By

The desk keeps moving briefly after the key is released, and the box rounds the
displayed height. Press Sit or Stand a few times:

- stops **past** the target → raise Stop Early By;
- stops **short** → lower it.

### How go-to-height works

The box has its own presets, but saving them over this bus was unreliable, so the
firmware ignores them. To go to a height it holds Up or Down while watching the
height on the display bus, and releases early by Stop Early By. It also:

- presses again if the desk hasn't started moving within 2 s (a sleeping box
  wakes on the first press and ignores it), up to 4 presses;
- re-presses every 10 s on long moves, since the box stops after ~15 s of one hold;
- gives up after 3 s without movement once it has moved (end of travel or an
  obstruction), or 40 s total.

Any manual Nudge or Stop cancels a move in progress.

## Home Assistant auto-stand

See [home-assistant/README.md](home-assistant/README.md). Short version: a
"Sitting at desk" template sensor (presence **and** desk at sitting height), an
automation that presses Stand after N minutes of it, waits M minutes, and presses
Sit only if you're still there and the desk is still up. Both times are helpers
you can change from the dashboard, and there's a toggle to turn it off.

## Repo layout

```
firmware/
  smart-desk.yaml         ESPHome config (secrets via !secret)
  desk_panel.h            bus slave + go-to-height logic
  secrets.yaml.example    template for your own secrets.yaml
home-assistant/
  desk_autostand.yaml     helpers, template sensor and automation as a package
  README.md               setup in the UI or as a package
docs/
  protocol.md             the handset bus: addresses, key codes, segment table
```

## Safety

This drives a motorised desk with no obstruction sensing beyond what the control
box already has. Keep cables, chairs and pets clear of it. Automations should
only move the desk when someone is at it, which is why the auto-stand checks
presence before both moves.

## Credits

- [dong4j/desk-gateway](https://github.com/dong4j/desk-gateway): the protocol
  reverse-engineering for the MXSK05_A handset.
- [ESPHome](https://esphome.io) and [Home Assistant](https://www.home-assistant.io).

## License

Public domain under [the Unlicense](LICENSE). Use it, change it, sell it; no credit needed.
