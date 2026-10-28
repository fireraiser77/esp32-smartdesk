# Handset bus protocol (MXKZH06 box ↔ MXSK05_A handset)

Most of this comes from [dong4j/desk-gateway's protocol notes](https://github.com/dong4j/desk-gateway/blob/main/docs/architecture/protocol-reverse-notes.md),
which cover the same MXSK05_A V1.4 handset. I confirmed it against logic
analyzer captures from my own desk before writing any firmware.

## Physical

The handset plugs into the control box with an RJ45-style cable. Four
conductors matter:

| RJ45 pin | Wire | Handset pad | Signal |
|---|---|---|---|
| 1 | red | (3.3V) | 3.3 V supply from the box |
| 2 | white | K | CLK |
| 3 | green | GND | ground |
| 4 | black | DAT | DAT |

Colours are what my cable used. Check yours against the pad labels on the
handset PCB before connecting anything.

Logic is 3.3 V, so an ESP32 connects directly. The handset has its own pull-ups
on CLK and DAT; with the handset unplugged the ESP32's internal pull-ups take
their place.

## Bus

The handset's chip is a **TM1650** (LED driver + key scanner). It speaks a two-wire
I2C-like protocol. The control box is the master and the handset is the slave.

- Clock is about **9.6 kHz**, and the box runs a full cycle about every **3.7 ms**.
- Each cycle the box:
  1. writes `0x01` to address `0x48` (display on), then reads one key byte from `0x49`;
  2. writes one segment byte each to `0x68`, `0x6A`, `0x6C`, `0x6E` (digits 1–4).
- START/STOP/ACK work as in I2C. The slave ACKs by pulling DAT low.

An ESP32 replacing the handset has to answer the `0x49` read with a key code and
ACK the display writes. The ESP32's hardware I2C peripheral can't easily act as
this slave, so [`desk_panel.h`](../firmware/desk_panel.h) bit-bangs it in a GPIO
interrupt (IRAM, pinned to core 1 away from Wi-Fi).

## Key codes

| Key | Byte |
|---|---|
| none | `0x2E` |
| Up | `0x47` |
| Down | `0x4F` |
| Preset 1 | `0x17` |
| Preset 4 | `0x2F` |
| Up + Down | `0x7F`: **factory reset, never send this** |

The box moves the desk for as long as the key is reported, and gives up after
about 15 s of one continuous hold. The firmware re-presses every 10 s on long moves.

## Display / height

The segment bytes use this board's own wiring, not the standard 7-segment table:

| Digit | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| Byte | `5F` | `44` | `9E` | `D6` | `C5` | `D3` | `DB` | `46` | `DF` | `D7` |

Bit `0x20` is the decimal point. Digits 1–3 carry the number (e.g. `27.0`);
digit 4 mirrors digit 3. A byte of `00` means the digit is blank.

## Behaviour worth knowing

- **Sleep.** About 10 s after the desk stops, the box blanks the display (all
  `00`). Holding a key wakes it; the display shows the height within about half a
  second and the desk usually starts moving within about 1.5 s. A quick tap may
  only wake it. The firmware remembers the last height it saw, and if a move
  hasn't started within 4 s it lets go and presses again (up to 4 times).
- **Bottom limit.** Pressing Down at the lowest height shows `07.0` instead of
  the height. The firmware ignores readings outside 18–60 in.
- **Any press stops a moving desk.** Including the box's own preset moves.
  That's why the firmware stops by simply releasing the key.
- **Coast.** The desk keeps moving a little after the key is released. The
  `Stop Early By` setting releases that far before the target.
