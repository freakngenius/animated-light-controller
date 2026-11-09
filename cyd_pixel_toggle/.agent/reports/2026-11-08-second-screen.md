# Evidence: second screen (colour and brightness sliders)

Date: 2026-11-08. Work type: feature. Intent: `.agent/intent.md` (approved by Kyle 2026-11-08).

## Gates (exit codes)
- `python3 .agent/bin/gates.py run`: host-tests PASS, firmware-compile PASS, exit 0 (after the review clean-ups).
- Host tests: physics, gesture (plus broken mode must fail), lifx_proto (110 checks), lifx_sim (102 runs), preview (dirty rects, 2 vs 4 bands), screens (64 checks), flows (103 checks).
- Canary: with `SPLIT_X` moved to 300 in a scratch copy, `test_flows.py` failed 9 checks, so it can fail. Gesture and LIFX tests have their own broken modes. `gates.py prove` cannot plant a C++ canary, so gates were proved by hand.

## Bugs the new tests found (both fixed)
1. Sun and moon icons on screen two did not repaint (drawn only when the slider frame was in the repaint rectangle).
2. The status dot was erased by full repaints of screen two and by the wipe.

## Acceptance walk
| Acceptance | Result |
|---|---|
| 10 swipe round trips, no miss or double | Host: yes (`test_flows`). Real board: NOT tried |
| Down-drag on brightness slider never leaves screen 2 | Host: yes. Board: not tried |
| Sideways drag on hue never leaves screen 2 | Host: yes. Board: not tried |
| Full spectrum, pixel steps, dark background | Host frames looked at (`scratchpad/sim/out/s2_start.png`). Board: not looked at |
| 25 fps or better on screen 2 and during the wipe | NOT measured. Screen one measured at 36 to 37 fps |
| Screen one matches baseline | Pixel-identical to before screen two (0 px differ); vs the original baseline, max channel error 17/255 (the earlier speed-up rounding) |
| Host tests pass, compile exits 0, heap above 20 KB | Yes; free heap 90,720 bytes after start-up |
| SetColor bytes in host test; at most about 10 packets/s | Yes (lifx_proto, lifx_sim) |
| Bulb follows hue and brightness, power unchanged | Host sim: yes. Real bulb: NOT tried |

## Judge, holdouts, review
- Scope judge: skipped, no API key in the environment.
- Holdouts: 3 drafted, not locked (Kyle must read and lock), so not run.
- Code review (medium/high): 8 findings; 5 fixed, 1 by design, 2 skipped (full repaint after the wipe is unmeasured; cyd_sim clutter).

## Known behaviour
Moving only brightness sends the full colour at the current hue, so a white bulb turns coloured.

## Not done
Real-board swipe/drag/fps/bulb checks; holdouts; board not reflashed with the review clean-ups; ai-memory checkpoint (needs Kyle's OK).
