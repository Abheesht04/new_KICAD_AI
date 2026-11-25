# Copper parts library (`copper_parts` schema 1)

Real, buyable parts for Copper's designs, one file per family. The format is `(copper_parts (schema 1) (part ...))`,
read by `src/v2_src/part_catalogue.c` and checked by `copper catalogue check` (docs/engineering/CLI.md).

## Rules (PIPELINE_HARD_RULES.md H2)

- Every value was copied from the **manufacturer's own document**: the URL, document number, revision and page are on
  each rating. Distributor listings were not used as sources.
- Everything here is **`unverified`**: it was transcribed by Copper's assistant on 2026-11-21. **Only a person** may
  change a source to `(status "verified") (verified_by "name") (verified_on "YYYY-MM-DD")`, after checking the value
  against the cited page.
- No value is estimated. A rating the datasheet does not state is left out, and `copper catalogue check` reports it.
- MCU strapping pins, reference circuits and other application data do not belong here. They go in the block data
  files (Phase 2), each with its own source.

## What a rating means

The schema has one rating name per quantity, so each part says in its `description` what a rating means for that
part:
- `voltage maximum` is VDS for a MOSFET, VRRM for a diode, VR for an LED, the input voltage for a regulator, and the
  contact rating for a switch.
- `current maximum` is ID for a MOSFET, IF(AV) for a diode, the DC forward current for an LED, the contact rating for a
  switch, and the lowest per-pin rating for a connector.
- `temperature minimum/maximum` is the operating or junction range named in the description.

## Check

```
copper catalogue check parts_library/<file>.parts --symbols-root <KiCad symbols> --footprints-root <KiCad footprints>
```

On 2026-11-21 (KiCad **10.0.5** libraries, a development check; KiCad 9 is the target) the result was **UNVERIFIED**:
nothing is verified by a person yet. One known FAIL: KiCad 10 stacks the ESP32-WROOM-32E's four GND pins into one
symbol pin `[1,15,38,39]`, so the pin map's GND entries do not match KiCad 10's symbol (G-033). Stacked pin numbers
are a KiCad 10 feature, so KiCad 9's symbol is expected to have separate GND pins. This is **not verified here**: no
KiCad 9 libraries are installed (G-010).

The ctest test `parts_library_parse` checks every file here without KiCad: each must parse and report UNVERIFIED or
PASS, never FAIL or INVALID_INPUT.

## Files

| File | Parts |
|---|---|
| `resistors.parts` | Vishay CRCW0603 thick-film, 1 %: 22 Ω, 100 Ω, 620 Ω, 680 Ω, 1 kΩ, 5.1 kΩ, 10 kΩ |
| `capacitors.parts` | Murata GRM: 10 nF 50 V X7R 0603, 100 nF 50 V X7R 0603, 1 µF 25 V X5R 0603, 10 µF 25 V X5R 0805, 22 µF 25 V X5R 1206; Vishay 293D 22 µF 10 V solid tantalum (B case) |
| `semiconductors.parts` | Kingbright APT1608EC red LED 0603, Vishay SS14 Schottky (SMA), Alpha & Omega AO3400A N-MOSFET (SOT-23) |
| `ics.parts` | AMS1117-3.3 LDO (SOT-223), Espressif ESP32-S3-WROOM-1-N8 (recommended; used by the blocks), Espressif ESP32-WROOM-32E-N4, TI TMP102 temperature sensor (SOT-563) |
| `electromechanical.parts` | E-Switch TL3342F160QG tactile switch, Samtec TSW-102/103-07-T-S headers, GCT USB4105-GF-A USB-C receptacle |
