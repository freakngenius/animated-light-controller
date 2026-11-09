# Pixel Toggle on the CYD: setup guide

Two screens, in the same pixel-art style.

- **Screen one:** tap the toggle and the moon and sun swap with a spring animation, and your LIFX bulb
  fades off (dark mode) or on (light mode) over your home Wi-Fi.
- **Screen two:** swipe **up** from screen one. The left two thirds hold a rainbow slider (the bulb's colour),
  the right third holds a vertical slider (the bulb's brightness). Swipe **down** over the left two thirds to come back.
  The two screens swap with a pixel-block wipe.

Screen two was flashed to the real board and boots, but nobody has yet swiped or dragged on it there. Section 12 lists
exactly what was and was not checked, so you know what to look at first.

---

## 1. What you need

- the CYD board (ESP32-2432S028R) and a USB cable that carries **data** (some cables only charge)
- a Mac with the Arduino IDE (2.x)
- your LIFX bulb, switched on at the wall and already working in the LIFX app
- Wi-Fi that is **2.4 GHz** (the ESP32 cannot join 5 GHz) and that is the **same network** as the bulb

## 2. Install the board support (one time)

1. Arduino IDE → **Settings** (Cmd+,) → find **Additional boards manager URLs** and paste:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. **Tools → Board → Boards Manager…**, search `esp32`, install **esp32 by Espressif Systems**.
   Pick version **2.0.17** from the version drop-down (that is the version this sketch was compiled with;
   you already have it).

## 3. Install the one library (one time)

**Sketch → Include Library → Manage Libraries…**, search **LovyanGFX** (author *lovyan03*), install **1.3.0**.

That is the only library to add. Wi-Fi and Preferences come with the board support.
You also have *TFT_eSPI* installed. This sketch does not use it, leave it alone: it does no harm.

## 4. Pick the board settings (every time you open the sketch)

Plug the board in, then in the **Tools** menu choose:

| Setting | Choose |
|---|---|
| Board | **ESP32 Dev Module** |
| Port | the one that looks like `/dev/cu.usbserial-…` (or `wchusbserial…`) |
| Upload Speed | **115200** (higher speeds often fail on this board's USB chip) |
| CPU Frequency | 240MHz (WiFi/BT) |
| Flash Frequency | 80MHz |
| Flash Mode | QIO |
| Flash Size | 4MB (32Mb) |
| Partition Scheme | Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS) |
| PSRAM | **Disabled** (this board has none) |
| Core Debug Level | None |

No port in the list? The board's USB chip (CH340) needs a driver on some Macs. Search for
"WCH CH34x macOS driver", install it, unplug and replug.

## 5. Put in your Wi-Fi and open the sketch

1. Keep the whole `cyd_pixel_toggle` folder together (the folder name must stay `cyd_pixel_toggle`).
   Put it in `Documents/Arduino/`.
2. Open `cyd_pixel_toggle.ino` in the Arduino IDE. All the other files open as tabs.
3. Open the **secrets.h** tab and type your Wi-Fi name and password between the quotes.
4. Optional: open **config.h** (colours, bulb IP, fade time, all explained in comments).

## 6. Upload

Click the **→ Upload** button. The first build takes a minute or two.
If the IDE says `Connecting........____` and fails: hold the **BOOT** button on the board,
click Upload again, and let go of BOOT when the percentages start.

Then open **Tools → Serial Monitor**, set the speed to **115200**. You will see what the board is doing.

## 7. First run and touch calibration

1. The screen shows **PIXEL TOGGLE** for about 1.6 seconds.
2. The very first time, **Touch calibration** starts by itself: tap the centre of each mark as it appears
   (4 corners). It is saved in the board's flash memory, so you only do this once. It survives power-off and re-uploading.
3. The toggle appears in dark mode. A moment later, if your bulb is on, the toggle slides to light by itself.

**To calibrate again:** during the splash (first 1.6 s after power-up or after pressing RESET),
either keep a finger on the screen or hold the **BOOT** button. It also recalibrates automatically if you change `CYD_ROTATION`.
(Another way: set `FORCE_TOUCH_CALIBRATION` to `1` in config.h, upload, calibrate, set it back to `0`, upload.)

> Do not hold BOOT while *pressing* RESET. That puts the chip into upload mode instead.
> Press RESET first, then hold BOOT during the splash.

## 8. If something looks wrong

| What you see | Fix |
|---|---|
| Colours look like a photo negative | `config.h`: change `CYD_VARIANT` 1→2 (or 3→4) |
| White screen, garbage, or nothing | `CYD_VARIANT` 1→3 (or 2→4): you have the other screen chip |
| Orange looks blue | `CYD_SWAP_RED_BLUE` → 1 |
| Picture upside down | `CYD_ROTATION` 1→3 (then the touch recalibrates itself) |
| Sparkles or flicker on the screen | `CYD_SPI_MHZ` 40→27 |
| "Not enough RAM for the picture buffer" | `SPRITE_BANDS` 2→4 |
| Touch lands in the wrong place | Redo the calibration (section 7) |
| Red dot top-right | Not on your Wi-Fi: check secrets.h, 2.4 GHz, signal |
| Amber dot top-right | On Wi-Fi but no bulb found: see below |

**Bulb not found (amber dot).** Some routers block the "is anyone there?" broadcast the sketch uses to find the bulb.
Find the bulb's IP address in your router's device list (or the LIFX app → the bulb's settings), and type it
into `LIFX_FALLBACK_IP` in config.h, for example `"192.168.1.42"`. Guest networks and "client isolation" settings also block it.

**Several LIFX lights?** The sketch uses the first one that answers. The Serial Monitor lists every LIFX device it
finds, like `LIFX device d073d5a1b2c3 at 192.168.1.42`. To pin one, copy its id (no colons) into `LIFX_TARGET_MAC`.

## 9. Test checklist

Tick these in order. Keep the Serial Monitor open: it explains what is happening.

**A. The screen draws**
- [ ] Splash "PIXEL TOGGLE" appears, then the toggle with a crescent moon in dark mode.
- [ ] Colours look right (dark blue-grey background, pale moon, a violet-blue glow on the knob's edge). If not, section 8.
- [ ] Serial Monitor shows `[draw] picture buffer: … bytes` and no "Not enough RAM" message.

**B. A tap flips the toggle and the bulb**
- [ ] Tap the toggle: the knob leans in under your finger, slides across with a little stretch and squash,
      the moon breaks into pixels that swirl and re-form as a sun, and the background turns light.
- [ ] The bulb fades **on** over about a third of a second. Serial: `[lifx] … on`.
- [ ] Tap again: back to dark, bulb fades **off**.
- [ ] Tap very fast, 8 to 10 times in a row: the animation never glitches or freezes, and the bulb ends up matching
      the final state of the toggle (give it a second).
- [ ] While animating, the Serial Monitor prints `[fps] …`. It should say 30 or more.

**C. The toggle follows the LIFX app**
- [ ] With the toggle in dark mode, turn the bulb **on** in the LIFX app. Within about 3 seconds the toggle slides to light by itself.
- [ ] Turn the bulb **off** in the app: the toggle slides back to dark.
- [ ] Tap the toggle, then immediately change the bulb in the app: no flip-flopping, it settles on the last thing that happened.

**D. The board recovers after Wi-Fi drops**
- [ ] Switch your router off for about 30 seconds (or turn off the phone hotspot, if you tested with one).
      A quiet **red dot** appears top-right. The toggle still animates when tapped; nothing freezes.
- [ ] Switch it back on. Within about 10 to 20 seconds the dot disappears by itself with no reset.
- [ ] Unplug the bulb for 30 seconds: **amber dot**. Plug it back in: the dot goes away, and the toggle matches the bulb.
- [ ] Tap while the bulb is unplugged, then plug it in within 20 seconds: the bulb gets your last tap.
      After 20 seconds the board gives up on that tap and follows the bulb instead.

**E. Screen two (colour and brightness)**
- [ ] Swipe up from screen one (anywhere, one clear stroke of about 1 cm). A wave of pixel blocks sweeps up and screen two appears in under half a second.
- [ ] The rainbow bar shows red, yellow, green, cyan, blue, magenta, red in visible pixel steps. The knob sits on the bar and its window shows the colour you chose.
- [ ] Drag the knob sideways: it follows your finger, leans in, stretches a little and settles with a spring when you let go. The bulb changes colour while you drag (at most about 10 updates a second) and the final colour arrives when you lift.
- [ ] Tap anywhere on the rainbow bar: the knob jumps there.
- [ ] Drag the right-hand slider up and down: the pixel meter lights from the bottom in the chosen colour, the sun warms up and the moon dims as it goes up. The bulb's brightness follows (1% at the bottom, 100% at the top).
- [ ] Swipe **down** starting on the right-hand slider: it only moves that slider and never leaves screen two.
- [ ] Drag the rainbow knob sideways while wobbling up and down a little: it never leaves screen two.
- [ ] Swipe down over the left two thirds (start anywhere left of the right-hand slider, including above and below the rainbow bar): the wave sweeps down and screen one is back.
- [ ] Do ten swipe-up / swipe-down round trips in a row: none missed, none doubled.
- [ ] The bulb **power** never changes while you use screen two. If the bulb is off it stays off; if it is on it stays on.
- [ ] Touch the screen during a wipe: nothing happens, nothing gets stuck, the next swipe works.
- [ ] Back on screen one the toggle still works and still flips the bulb.

Known behaviour: moving only the brightness slider sends the full colour at the current hue, so a bulb that was
showing plain white turns coloured (blue at first, until you pick a hue). There is no saturation control on purpose.

## 10. Why it is built this way

**LovyanGFX instead of TFT_eSPI.**
- LovyanGFX keeps the screen and touch wiring in your sketch (`display_config.h`). TFT_eSPI needs you to edit
  `User_Setup.h` *inside the library folder*, which gets wiped on every library update and is easy to get wrong.
- LovyanGFX has first-class off-screen buffers ("sprites") and sends them to the screen with DMA.
- It includes the XPT2046 touch driver on its own separate pins, and a built-in calibration routine. TFT_eSPI would need a second library.

**Scale 70%** (not 75%): the 392 × 198 design becomes 274 × 139 pixels. That leaves 23 pixels of breathing room
left and right and about 50 above and below. At 75% the glow would touch the screen edges.

**No flicker.** Each frame is drawn into a hidden buffer in RAM and then sent to the screen in one go. The buffer only
covers the toggle and its glow (294 × 159 pixels), not the whole screen. At 2 bytes per pixel that is **93,492 bytes** if
kept in one piece. The default (`SPRITE_BANDS 2`) cuts it into two strips of about 47 KB each, and keeps **two** such
buffers: while one strip is being sent to the screen (by DMA, in the background) the CPU already paints the next one into
the other. The other numbers are in section 11. The ESP32 on this board has no extra PSRAM, and Wi-Fi needs a large share
of what is left, so the buffers are reserved first, before Wi-Fi starts.

**Why it is fast enough.** Measured on the real board, the first version drew at 8 fps. Three things fixed that:
the Arduino build compiles with `-Os` (smallest code), so `draw.cpp` now asks for `-O3` (speed); the neon rim and the
ring around the track are painted with whole-number maths and lookup tables instead of per-pixel floating point (the
chip has no fast square root or division); and the strips are sent in the background while the next one is painted.

**Fast, repeated taps never glitch.** The springs run in fixed 1/480-second steps whatever the frame rate. Every tap
cancels the delayed events of the previous tap, so a stale timer cannot fire into the new animation. Particles restart their
journey from wherever they are, and the rim glow keeps turning the same way. This is tested on a laptop with 300 random tap storms.

**The network never blocks the animation.** Wi-Fi and the bulb live on their own task on the other CPU core. The animation
loop only reads a couple of numbers from it.

## 11. Memory and speed numbers

**Picture buffer (the hidden drawing area, 294 × 159 pixels, 2 bytes per pixel):**

| `SPRITE_BANDS` | buffer in RAM |
|---|---|
| 1 | 93,492 bytes (about 91 KB), one buffer: no overlap of drawing and sending, slower |
| **2 (default)** | **94,080 bytes (about 92 KB)**: two 294 × 80 strips (47,040 bytes each), the strips take turns |
| 4 | 47,040 bytes (about 46 KB): two 294 × 40 strips |

Two buffers are kept in RAM (one when `SPRITE_BANDS` is 1) so a strip can go to the screen while the next is painted.
A small cache (about 29 KB) holds the fixed shape of the ring around the track. The measured free heap with Wi-Fi up is
about 91 KB, which is plenty for Wi-Fi and the bulb. If memory is ever short, `SPRITE_BANDS 4` saves about 47 KB.

**Everything else (from the real build):**
- Program: 855,977 bytes, 65% of the 1.31 MB program space.
- Fixed RAM (globals): 58,424 bytes, 17% of 320 KB. That includes the Wi-Fi library and the animation state (about 9.4 KB for the 87 pixel-cell particles).
- Wi-Fi/LIFX task: 8 KB stack + a 1.5 KB network buffer, allocated after the picture buffer.
- On every start the Serial Monitor prints the free heap and the largest free block. After the picture buffer is reserved there should be well over 100 KB left for Wi-Fi.

**Speed (measured on the real board, Wi-Fi and bulb running):**

| Version | frames per second while the toggle moves |
|---|---|
| First version | 7 to 10 |
| Built for speed (`-O3`) | 13 to 15 |
| Rim and ring rewritten, strips sent in the background | **31 to 47** (typically 35 to 37; 31 to 32 during a fast burst of taps) |

Where the time goes now, per frame while moving: the neon rim about 11 ms, the knob 4 ms, the ring around the track
3.5 ms, the knob's shadow 2.5 ms, the track 1.2 ms, everything else about 2 ms. The screen takes about 19 ms to receive
the whole 294 × 159 area at 40 MHz, but that happens in the background while the next frame is painted. The one slower
frame is the one where the page background outside the toggle is repainted (about 20 ms of sending that cannot overlap).
When nothing is moving in light mode, the sketch sends nothing at all. The numbers print on the Serial Monitor as
`[fps] … draw … us, send … us` every 2 seconds while the toggle animates.
(Lowering `CYD_SPI_MHZ` would make it slower, not faster. To re-measure, set `CYD_BENCH` to 1 in config.h: the toggle then
flips itself every 3 seconds and prints the time of every drawing layer. Set it back to 0 afterwards.)

## 12. What was checked, and what was not

**Screen two (added later):**
- Checked on the laptop (exit codes verified): the gesture recogniser (`build_gesture_test.sh`, broken mode fails on purpose),
  slider physics, screen two partial repaint against clean repaint over 420 frames, the pixel-block wipe in both directions
  (every pixel is old screen or new screen, last frame equals a clean repaint, fast and slow frame steps), 103 checks that drive the
  real app code with scripted touches (`test_flows.py` through `cyd_sim.sh`), SetColor packet bytes and 102 simulated runs of the
  colour sender (at most 10 packets a second, newest value always wins, power never touched). The flow tests were shown to fail
  when the right-hand zone line was moved. These tests found and fixed two real bugs (sun/moon icons not repainting, status dot erased by repaints).
- Screen one frames from the host preview are pixel-identical to before screen two was added.
- The sketch compiles for the ESP32 and was flashed. On the board: it boots, joins Wi-Fi, finds the bulb, free heap 90,720 bytes, screen one 36 to 37 fps.
- **Not checked on the board yet:** swiping between the screens, dragging the sliders, the frame rate on screen two and during the wipe,
  and the bulb following the sliders. Use checklist E. The board currently runs the build from just before the code-review clean-ups (compiled, not reflashed).
- Not run: the hidden holdout scenarios (not locked yet) and the scope judge (needs an API key).

**Checked on the laptop (all pass; exit codes verified):**
- Physics: 300 random tap storms, 600 ms stalls, 4,000 flips in a row, 1 hour idle. The toggle always ends exactly on the right resting pose, with no NaN or runaway numbers
  (`cyd_pixel_toggle_tests/build_physics_test.sh`). The test failed before I fixed a float-precision bug, so it can catch problems.
- LIFX packets: byte-for-byte against the layouts in the official LIFX LAN documentation, malformed and random packets
  (`build_lifx_test.sh`; `build_lifx_test.sh broken` fails on purpose).
- The real LIFX task code ran against a fake bulb and a fake clock: discovery, lost packets, fade and verify, bulb disappearing and returning,
  IP change, Wi-Fi drop, clock wrap-around, a 29-minute run (`build_lifx_sim.sh`).
- Drawing: frames rendered on the laptop and looked at (`build_preview.sh`), partial screen updates compared against full redraws, 2-strip against 4-strip.
- The whole sketch compiles for the ESP32 (Arduino core 2.0.17, LovyanGFX 1.3.0) with no warnings from these files.

**Not checked, because I have no board and no bulb. Look at these first:**
- The screen variant. Which of `CYD_VARIANT` 1 to 4 your board needs (section 8).
- The screen direction and touch orientation (`CYD_ROTATION`, calibration).
- (Measured since: the frame rate is 31 to 47 fps while moving, see section 11. The bulb follows taps and confirms.)
- Whether the animation looks right on the real panel after the speed-up (the laptop frames match the earlier ones to within one colour step).
- That the picture buffer gets its memory before Wi-Fi starts. The Serial Monitor tells you.
- The LIFX side with a real bulb: broadcast discovery on your router, the fade, acknowledgements. About 30 seconds after start the Serial Monitor
  prints the Wi-Fi task's stack use; if it says it came close to 8192 bytes, tell me.
- Touch feel: debounce timing on your particular screen.
