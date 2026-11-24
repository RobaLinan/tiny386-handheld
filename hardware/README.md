# Handheld hardware

Mechanical and electrical design for the ESP32-P4 handheld (`jc4880p443` build).
Everything here is optional: the firmware runs on the bare module with an SD
card, a USB cable and a display.

```
pcb/   carrier PCB: 3x4 key matrix + joystick header, U-shaped around the screen
cad/   FreeCAD case and 3MF print exports
```

## PCB

`pcb/tiny386-handheld.eprj2` is the EasyEDA Pro project; `pcb/gerber.zip` is the
fabrication data (`3D_PCB1_2026-11-10`), `pcb/pcb-3d.step` the 3D export and
`pcb/matrix-3x4.png` the key-matrix schematic page.

- 211.6 x 89.1 mm, 1.6 mm FR-4, U-shaped with a 120 x 75 mm opening for the
  4.3" JC4880P443 module.
- 12 x 6 mm silent tact switches at 5 mm pitch; one 1N5817 diode per key
  (anode on the row side) for a row-high scan with no ghosting.
- J_ROW (3 pins), J_COL (4 pins) and J_GND 2.54 mm headers; 4 x M3 NPTH
  (3.2 mm) mounting holes.
- The module plugs in through the 2x13 P1 header; no other connectors.

Matrix wiring to P1 (P1 pin numbers on the module):

| Signal | Module pin | P1 pin |
|---|---|---|
| Row 1 | GPIO28 | 21 |
| Row 2 | GPIO29 | 14 |
| Row 3 | GPIO30 | 12 |
| Col 1 | GPIO31 | 10 |
| Col 2 | GPIO32 | 19 |
| Col 3 | GPIO33 | 8 |
| Col 4 | GPIO52 | 7 |
| GND | GND | 5, 6 or 16 |

Rows are driven high one at a time (10 ms poll, 3-sample debounce in
`esp/main/gamepad.c`); columns have pull-downs. On this PCB revision the Down
and Left switches are wired to each other's columns, which the firmware
compensates for in `pad_map[]` — do not "fix" the table when editing the scan
code.

P1 pinout for reference (odd pins on one row, even on the other):

| odd | signal | even | signal |
|---|---|---|---|
| 1 | 3V3 | 2 | 5V |
| 3 | 3V3 | 4 | 5V |
| 5 | GND | 6 | GND |
| 7 | GPIO52 (Col 4) | 8 | GPIO33 (Col 3) |
| 9 | GPIO51 / ADC2_CH2 (battery) | 10 | GPIO31 (Col 1) |
| 11 | GPIO50 (free) | 12 | GPIO30 (Row 3) |
| 13 | GPIO49 (free) | 14 | GPIO29 (Row 2) |
| 15 | GPIO35 (BOOT) | 16 | GND |
| 17 | GPIO34 (free) | 18 | 3V3 |
| 19 | GPIO32 (Col 2) | 20 | C6 U0RXD |
| 21 | GPIO28 (Row 1) | 22 | C6 U0TXD |
| 23 | ES_I2C_SDA (GPIO7) | 24 | C6 IO9 |
| 25 | ES_I2C_SCL (GPIO8) | 26 | C6 CHIP_PU |

## Joystick

M5Stack Unit Joystick2 (U024-V2, STM32G030 + hall effect, I2C address `0x63`).
It is a 5 V Grove unit with its own 3.3 V LDO and 10k I2C pull-ups; the I2C
lines idle at 3.3 V and share the audio/touch bus.

| Grove wire | Signal | P1 pin |
|---|---|---|
| Black | GND | 5, 6 or 16 |
| Red | 5V (never 3V3) | 2 or 4 |
| Yellow | SDA (GPIO7) | 23 |
| White | SCL (GPIO8) | 25 |

The bus is shared with the ES8311 codec (`0x18`) and the GT911 touch panel
(`0x5D`). Registers used by the firmware: `0x00`-`0x03` raw X/Y (16-bit LE),
`0x20` stick button (0 = pressed), `0x50`-`0x53` factory-calibrated signed
12-bit offsets, `0xFE` firmware version.

The module is mounted rotated 90 degrees on this PCB, so the firmware swaps the
module's X/Y in `gamepad.c` (`joy_read`). If a direction still feels inverted
after assembly, set `InvertX`/`InvertY` in the `[gamepad]` section of
`/sdcard/menu.ini` instead of editing the driver.

## Battery and power

- 1S Li-ion on CN4: pin 1 = BAT+, pin 2 = BAT- (tied to board GND). There is
  no reverse-polarity protection — check the polarity before plugging in.
- IP5306 charger + 5 V boost powers the whole board. The third key is the
  IP5306 KEY: short press = on, hold ~10 s = off. Any USB connection keeps the
  output on, so unplug both Type-C ports before trying to power off.
- Battery gauge: 10k/10k divider from BAT+ to GND, midpoint to P1 pin 9
  (GPIO51 / ADC2_CH2). The firmware averages it and draws a percentage in the
  top-right corner of the picture.

## Case

`cad/handheld-case.FCStd` is the FreeCAD source; print exports are
`case-front.3mf`, `case-back.3mf`, `case-button.3mf`, `case-dpad.3mf` and
`case-battery-cover.3mf` (any 3MF-capable slicer). The 18650 holder model used
for the battery bay is a third-party design and is not redistributed here; use
your own holder or its original source.

The design files follow the repository license (BSD-3-Clause) unless noted.