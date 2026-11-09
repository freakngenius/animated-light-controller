# Animated Light Controller

A pocket light controller for the **ESP32-2432S028R "Cheap Yellow Display" (CYD)**: a 2.8" 320 x 240 resistive touch
screen with a pixel-art look and springy, squash-and-stretch animation. It controls a **LIFX** bulb over your home Wi-Fi.

## What it does

**Screen one: power.** Tap the pixel toggle. The moon and sun swap with a spring animation, and the bulb fades off (dark mode)
or on (light mode). The toggle also follows the bulb if you change it in the LIFX app.

**Screen two: colour and brightness.** Swipe **up** for a dark screen with two 8-bit sliders:

- the left two thirds: a rainbow bar that sets the bulb's **hue**
- the right third: a pixel meter that sets its **brightness** (1% to 100%)

Swipe **down** over the left two thirds to go back. The screens swap with a pixel-block wipe.
Sliders change the colour only; power stays with screen one.

Known quirk: moving only the brightness slider sends full colour at the current hue, so a plain white bulb turns coloured.
There is no saturation or white-temperature control.

## Hardware

- ESP32-2432S028R (CYD), ST7789 or ILI9341 panel variant (set `CYD_VARIANT` in `config.h`)
- a LIFX bulb on the same 2.4 GHz network
- a USB cable that carries data

## Quick start

```bash
cd cyd_pixel_toggle
./flash_cyd.sh
```

The script installs `arduino-cli`, the ESP32 core and LovyanGFX if they are missing, asks for your Wi-Fi name and password
(written to `secrets.h`, which is git-ignored), builds, and uploads. Prefer the Arduino IDE? See [`cyd_pixel_toggle/SETUP.md`](cyd_pixel_toggle/SETUP.md)
for board settings, touch calibration, troubleshooting and a test checklist.

Never commit `secrets.h`. `secrets.h.example` is the placeholder that ships with the repo.

## Repository layout

| Folder | What is in it |
|---|---|
| `cyd_pixel_toggle/` | The Arduino sketch (keep the folder name: it must match the `.ino`). Drawing, physics, gestures, screens, LIFX networking. |
| `cyd_pixel_toggle_tests/` | Laptop tests. They run the real drawing, gesture, screen and LIFX code against mocks, so no board is needed. |
| `cyd_pixel_toggle/.agent/` | Build notes: the intent for the second screen, the gate runner, an evidence report. |

## Tests (on your computer, no board needed)

```bash
sh cyd_pixel_toggle_tests/run_all.sh
```

Runs physics, gesture, LIFX packet and LIFX network simulations, drawing previews, screen two and the wipe, and scripted touch flows.
Exit code 0 means everything passed. `cyd_pixel_toggle_tests/cyd_sim.sh SCRIPT OUTDIR` plays a touch script through the real app code and saves
PNG frames; see the top of `cyd_sim.cpp` for the commands. Details in [`cyd_pixel_toggle_tests/README.md`](cyd_pixel_toggle_tests/README.md).

## Status

- Screen one: measured on a real board at 31 to 47 fps with a real bulb.
- Screen two: flashed and boots on a real board; swipes, slider drags, frame rate on screen two and bulb colour following have **not yet been
  checked on hardware**. Everything is covered by the laptop tests. Section 12 of `SETUP.md` has the full list.
