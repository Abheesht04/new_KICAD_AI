# Board renders

Routed boards of the two composed golden boards (`manus/golden/esp32s3_sensor`, `manus/golden/esp32s3_leds`), as of
Step 3e (commit `833807d`). They are the boards whose hashes `manus/golden/baseline.json` records.

| File | Board | Routing | KiCad DRC |
|---|---|---|---|
| `Esp32S3LedSensor.png` / `.svg` | ESP32-S3 + LED + TMP102 (SOT-563), 70 × 60 mm | 11 nets, 214 segments, 15 vias, 0 unconnected | 0 errors, 1 warning (G-085) |
| `Esp32S3Leds.png` / `.svg` | ESP32-S3 + 2 LEDs + reset/boot, 70 × 60 mm | 11 nets, 192 segments, 14 vias, 0 unconnected | 0 errors, 1 warning (G-085) |

Colours: red F.Cu, blue B.Cu (circles are vias), yellow F.SilkS, grey the board outline. The empty area above the
module is its antenna keep-out zone.

How they were made: `kicad-cli pcb export svg --layers F.Cu,B.Cu,F.SilkS,Edge.Cuts --mode-single
--exclude-drawing-sheet --page-size-mode 2` on the `golden_boards.ps1` output, then the SVG drawn to PNG at
20 px/mm with PyMuPDF. KiCad 10.0.5: a development check, not KiCad 9 evidence. Images only: not approved for
manufacturing (H8).
