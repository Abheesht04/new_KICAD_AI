# Copper block library (`copper.block/1`)

Circuit blocks that the composer (Phase 2c) joins into a design. The format is described in
`docs/engineering/BLOCK_FORMAT.md`. Every part comes from `parts_library/`, and every value, range and design note
names its source (PIPELINE_HARD_RULES.md H2). Check a block with:

```
copper block check blocks_library/<name>.block.json --parts parts_library/<file>.parts [--parts ...]
```

ctest `blocks_library_valid` requires every block here to be VALID against the whole parts library.

| Block | What it is | Main sources |
|---|---|---|
| `power_input_header` | 2-pin header DC input + 10 µF at the power entrance | Espressif ESP32-S3 HW guidelines p. 8 |
| `usb_c_sink` | USB-C (USB 2.0) receptacle, 5.1 kΩ Rd on CC1/CC2, D+/D−, shield to GND, 10 µF | USB Type-C spec R2.0 table 4-25 (p. 236), §4.8.1.2 (p. 226); GCT USB4105 drawing |
| `ldo_3v3` | AMS1117-3.3 + 22 µF **solid tantalum** output capacitor | AMS1117 datasheet p. 2–4 |
| `esp32s3_core` | ESP32-S3-WROOM-1-N8 + 22 µF + 0.1 µF, EN RC (10 kΩ / 1 µF), 22 Ω USB series resistors; ports EN, IO0, USB, UART0, 10 GPIOs | ESP32-S3-WROOM-1 datasheet v1.8 (fig. 9-1 p. 41, table 6-2 p. 27, table 3-1 p. 10); HW guidelines §1.3.13 |
| `reset_boot` | RESET (EN) and BOOT (IO0) switches, 0.1 µF on EN, IO0 pull-up | datasheet fig. 9-1; HW guidelines §1.3.9 |
| `led_indicator` | red 0603 LED + series resistor sized by formula | Kingbright APT1608EC p. 2; Ohm's law |
| `i2c_sensor_tmp102` | TMP102 at 0x48, 0.01 µF bypass, SCL/SDA pull-ups sized by formula | TI TMP102 SBOS397I; TI SLVA689A |

## Honest limits, stated in the blocks' own notes

- **ESD protection:** Espressif suggests an ESD diode at the power entrance. The library has no ESD part yet, so it is
  **not included**.
- **AMS1117 input voltage:** it needs at least 4.6 V (3.3 V + 1.3 V dropout at 0.8 A). On USB 5 V, at the conservative
  4.0 V lower bound, the input can be too low. The composer must report this rather than hide it.
- **Board layers:** Espressif recommends a **four-layer** PCB for ESP32-S3. Copper makes two-layer boards without a
  ground plane (G-050).
- **Values that are design choices, not datasheet values:** the LED current default (2 mA) and the GPIO0 pull-up
  (10 kΩ). Both are marked as such.
- **Pull-ups need a bus capacitance:** the I²C pull-ups cannot be sized until a bus capacitance is given. It is never
  assumed.
