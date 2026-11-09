# Tests for the CYD pixel toggle (they run on your Mac, no board needed)

Keep this folder NEXT TO the sketch folder, not inside it. The Arduino IDE would try to compile everything inside the sketch folder.

| Script | What it checks |
|---|---|
| `./build_physics_test.sh` | springs, tap storms, 4,000 flips, particle tables (`physics.cpp`) |
| `./build_lifx_test.sh` | LIFX packet bytes vs the official docs, malformed packets. `./build_lifx_test.sh broken` must FAIL (proves the test can fail) |
| `./build_lifx_sim.sh` | the real `lifx.cpp` against a fake bulb and fake clock |
| `./build_preview.sh` | draws the toggle on a fake screen and writes PNG frames (needs no libraries; `mock/LovyanGFX.hpp` stands in for the real one) |

Each script exits 0 when everything passes.
