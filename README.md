# Analog Watch

A watch that tells time using an analog voltmeter needle driven by a DAC. No screen. No hands. Just vibes and a carefully calibrated lookup table.

Featured in: **[I Built a Digital Watch With No Screen](https://youtu.be/faMIdIe0c6w)** (YOUTUBE LINK)

---

## What is this?

It's a watch. Technically. A Raspberry Pi Pico reads the time and drives an MCP4726 DAC, which pushes a voltage to a panel meter whose needle sweeps to represent whatever time-related concept you've most recently asked it about.

By default it shows seconds. Because why would you want to know the hours.

---

## How to read the time

Press a button.

| Button | Single Press | Double Click |
|--------|-------------|--------------|
| Top | Hours | Date |
| Mid | Month | Seconds |
| Low | Day of week | — |

The needle will swing to the correct position. Stare at it. Contemplate what you've built.

---

## Repo contents

| Path | What's in it |
|------|-------------|
| `ANALCLOCK_REV0_5.ino` | Arduino sketch (RP2040) |
| `analclock v1/` | KiCad PCB project — main board revisions 1 & 2 |
| `analclock v1/analog_dial/` | KiCad PCB project — the meter dial board |
| `analclock v1/gerberv1/` | Gerber files for the main board |
| `analclock v1/analog_dial/analogdial1gerbers/` | Gerber files for the dial board |
| `*.svg` / `*.dxf` | Watch face artwork |
| `*.step` / `*.stl` / `*.f3d` | 3D printed parts |

---

## Calibration

The meter needle is non-linear, so the firmware uses a cubic polynomial LUT to correct for this. You can tune it over serial:

```
dac <value>      — hold needle at a fixed DAC value
max <value>      — set full-scale and rebuild the LUT
set <idx> <val>  — manually poke a LUT entry
lut              — print current LUT
tz <offset>      — set UTC offset in hours
<unix timestamp> — set the time
```

