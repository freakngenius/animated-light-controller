# Intent: CYD second screen with colour and brightness sliders

Work type: feature

## Why
Part one gave the CYD board a pixel-art dark/light toggle that flips a LIFX bulb on and off. Kyle now wants a second screen in the same style and with the same springy animation: "a horizontal slider that goes through the full color spectrum" on the left two thirds, "a vertical slider for brightness" on the right third, on a dark background, in an 8-bit look. Swipe up on screen one opens it; swipe down over the left two thirds brings you back.

## Building
- Swipe up anywhere on screen one and screen two comes in with an animated transition. A plain tap on the toggle still flips the bulb exactly as before.
- Screen two has a dark background. The left two thirds hold a horizontal hue slider that runs through the full colour spectrum. The right third holds a vertical brightness slider.
- Both sliders are drawn in the 8-bit pixel style: chunky pixel track, pixel knob, stepped colour bands, and the same squash, spring and rim-light motion as the toggle knob.
- Dragging a slider knob moves it with the finger and the knob settles with a spring when released. The tap on a track also moves the knob there.
- The bulb follows the sliders. Hue and brightness changes are sent to the LIFX bulb while the finger drags, throttled, and the final value is sent when the finger lifts. The bulb power state is not touched.
- Swipe down starting in the left two thirds of screen two returns to screen one with an animated transition. A downward drag that starts on the brightness slider never changes screens; it only moves the slider.
- On the hue slider, a mostly sideways drag moves the knob and a mostly downward drag is the swipe back. The first few pixels of movement decide which.
- Screen one looks and behaves the same as it does today, and stays at the speed measured today.

## Not building
- White temperature or saturation control because Kyle asked only for hue and brightness sliders.
- A third screen or a screen list because the request names exactly two screens.
- Following colour or brightness changes made in the LIFX app because it needs the Get and State messages and polling that Kyle did not ask for.
- Saving slider positions across a reboot because nothing in the request mentions persistence.
- Power control from screen two because the toggle on screen one already owns on and off.
- Swipe left or right between screens because Kyle specified only up and down.
- Support for more than one bulb or a bulb picker because part one already finds the single bulb on its own.

## Slices
### S1 Swipe between two screens with a hard cut
- U1 Add a hardware-free gesture recogniser that turns raw touch samples into tap, swipe up, swipe down and drag with position, and feed it from the touch driver (files: gesture.h, gesture.cpp, touch.h, touch.cpp) [parallel-safe: no]
- U2 Add a hardware-free screen controller that owns which screen is showing and reacts to gestures, and move the screen logic out of the sketch file so the laptop can run it (files: screens.h, screens.cpp, cyd_pixel_toggle.ino) [parallel-safe: no]
- U3 Add screen two as a dark placeholder drawn by the real draw code (files: draw.h, draw.cpp) [parallel-safe: no]
- U4 Add a laptop driver, `cyd_sim`, that feeds scripted touch events through the real gesture, screen, physics and draw code and writes PNG frames plus a one-line state report (files: ../cyd_pixel_toggle_tests/cyd_sim.cpp, ../cyd_pixel_toggle_tests/cyd_sim.sh) [parallel-safe: no]
Done when: swiping up on screen one shows a dark screen two, swiping down in its left two thirds shows screen one again, a tap on the toggle still flips the bulb, and `cyd_sim` reproduces all of that on the laptop.

### S2 The two 8-bit sliders, drawn and draggable
- U1 Draw the hue slider (left two thirds) and the brightness slider (right third) in pixel style, and add their spring physics (files: draw.cpp, draw.h, physics.h) [parallel-safe: no]
- U2 Wire drag and tap to both sliders and the direction rule between hue drag and swipe back (files: cyd_pixel_toggle.ino, touch.cpp) [parallel-safe: no]
Done when: both knobs follow a finger, settle with a spring, never leave their tracks, and a downward swipe on the brightness slider only moves that slider.

### S3 Animated transition between screens
- U1 Replace the hard cut with a pixel-block transition in both directions, driven by the same frame loop (files: draw.cpp, draw.h, cyd_pixel_toggle.ino) [parallel-safe: no]
Done when: both directions animate smoothly in under half a second with no tearing, and a touch during the transition is ignored or queued without a stuck state.

### S4 The bulb follows the sliders
- U1 Add the LIFX SetColor message, a throttled setColor call and host tests for the packet bytes (files: lifx_proto.h, lifx_proto.cpp, lifx.h, lifx.cpp) [parallel-safe: yes]
- U2 Connect the slider values to setColor and show the link state on screen two (files: cyd_pixel_toggle.ino, draw.cpp) [parallel-safe: no]
Done when: dragging the hue slider changes the real bulb colour and dragging the brightness slider changes its brightness, with no flood of packets and no change to the power state.

## Acceptance
- On the real board, swipe up from screen one reaches screen two, and swipe down in the left two thirds of screen two reaches screen one, ten times in a row with no missed or doubled swipe.
- A downward drag that starts on the brightness slider never leaves screen two.
- A sideways drag on the hue slider never leaves screen two.
- The hue slider shows the full spectrum from red through yellow, green, cyan, blue and magenta back to red, in visible pixel steps, on a dark background.
- Frame rate stays at 25 frames per second or better while a slider moves and while a transition runs, as logged by the existing fps line on the serial port.
- Screen one frames from the host preview match the earlier baseline frames within the earlier measured rounding error.
- The host tests build and pass, the arduino-cli compile exits 0 for the normal build, and free heap after start-up is logged and stays above 20 KB.
- The LIFX packet for SetColor matches the documented byte layout in a host test, and slider drags send at most about ten packets per second.
- The bulb follows hue and brightness on screen two and stays in the same power state.

## Open questions
- none. Kyle chose the pixel-block wipe for the transition and chose to drive the real bulb (2026-11-08).

## Approval
Approved by Kyle: 2026-11-08
