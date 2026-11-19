# Copper workspace: instructions for the Manus agent

You are working on the user's own Windows PC through Manus Studio ("Work locally"), inside this folder. Copper is a local
electronics engineering engine: deterministic C programs plus a strict design compiler. Your job is to **propose**
designs and run Copper's checks. **Copper decides** whether something passes; you report what it says.

**No guessing.** Every part, pin number, pin name and pad you write must come from the `context` output. Every result
you report must come from a tool. Copper's grounding gate rejects any design detail that does not exist in the
installed KiCad libraries. Copper's claim check flags any statement in your answer that its facts do not support.
When you do not know something, say "unknown" and ask the user.

## Use only these commands

Run everything through `copper.ps1` in this folder, from PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\copper.ps1 <command> [options]
```

| Command | What it does |
|---|---|
| `doctor` | Shows which tools are installed and the KiCad version. Run it first. |
| `context -Prompt "<parts>" -Out designs\<name>\context.json` | Reads the **real** symbol pin numbers, pin names (and electrical types) and footprint pad numbers of the parts named. |
| `check -Intent designs\<name>\intent.json` | **Grounding gate.** Checks every part, pin, pin name, pad and net against the installed libraries. GROUNDED or REJECTED, with one finding per problem. |
| `compile -Intent designs\<name>\intent.json -Out designs\<name>` | Runs the gate again, then compiles a GROUNDED intent into a KiCad project and runs the netlist, ERC, DRC and Copper import. |
| `checks -Project designs\<name>\<ProjectName>` | Summarises the ERC and DRC reports that `compile` saved. |
| `inspect -Board <file.kicad_pcb>` | Board summary: footprints, pads, nets, tracks, and anything Copper could not model. |
| `dfm -Board <file.kicad_pcb> [-Profile <fab profile json>]` | Checks manufacturability against JLCPCB's published 2-layer limits (default). |
| `topology -Schematic <x.kicad_sch> -Netlist <x.kicad_net>` | Pin-to-net table of a schematic. |
| `report -Project designs\<name>\<ProjectName>` | Writes **REPORT.md**, the facts sheet built only from tool output, plus `copper_facts.json`. |
| `claimcheck -Text designs\<name>\answer.txt -Project designs\<name>\<ProjectName>` | Checks your draft answer against the facts and lists every statement they do not support. |
| `workbench` | Opens the Copper Workbench browser page for the user, at http://127.0.0.1:8765. |

Every command ends with one line `COPPER_RESULT {...}`. Read `exit` and `outcome` there, and report them.

## Workflow: "design a board for ..."

1. **Check the tools.** Run `doctor`. If something is MISSING, stop and tell the user what is missing.
2. **Choose standard parts.** Use standard KiCad parts: `Device:R`, `Device:C`, `Device:LED`,
   `Connector_Generic:Conn_01x02`, and the SMD footprints `Resistor_SMD:R_0603_1608Metric`,
   `Capacitor_SMD:C_0603_1608Metric`, `LED_SMD:LED_0603_1608Metric` and
   `Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical`. If the request needs a part you cannot find this
   way, ask the user; do not invent an ID.
3. **Get the real pin data.** Run `context` with a text that names each part as `symbol <Lib:Name>` and
   `footprint <Lib:Name>`; the words `symbol` and `footprint` are required. For example:
   `-Prompt "symbol Device:LED footprint LED_SMD:LED_0603_1608Metric symbol Device:R footprint Resistor_SMD:R_0603_1608Metric"`.
   - Open the context JSON. For every pin, copy its `number` into `symbol_pin` and its `name` into `pin_name`. Take
     the `pad` from the footprint's pads.
   - For LEDs and diodes the names `K` (cathode) and `A` (anode) tell you the polarity. Never assume pin 1 is the
     anode.
   - An unnamed pin (a resistor's) has the name `""` or `~`.
   - If a part is missing from the context, its ID is wrong. Fix the ID before going on.
4. **Write the intent.** Write `designs\<name>\intent.json` following `copper.design-intent.v1.schema.json` (in this
   folder). `examples\led_indicator.intent.json` is a complete, grounded example.
5. **Run the gate.** Run `check`.
   - **REJECTED:** fix every listed finding using the context data, then check again. Never "fix" a finding by
     inventing another value.
   - Continue only when the result is **GROUNDED**.
6. **Compile.** Run `compile`. Exit codes:
   - `0`: the project was generated. **The board is not routed**, and ERC and DRC may still have findings.
   - `2`: the gate or the compiler rejected the intent. Read the findings or the `ERROR:` line, fix the intent, and
     compile into a **new** folder.
   - `3`: KiCad could not export the netlist.
   - Any other value: Copper's import check failed (read its message).
7. **Run the checks.** Run `checks` on the generated project folder, then `inspect` and `dfm` on its `.kicad_pcb`.
8. **Write the facts sheet.** Run `report` on the project folder. It writes `REPORT.md`.
9. **Draft your answer.** Write your draft to `designs\<name>\answer.txt`:
   - start with the full text of `REPORT.md`, unchanged;
   - you may add explanations after it, but anything that is your own suggestion and not a tool result must be
     marked "Suggestion (not checked by Copper):".
10. **Check your answer.** Run `claimcheck` on the draft. If it lists any statement, rewrite or remove it and run
    `claimcheck` again. Send the answer only when `claimcheck` reports `NO_UNSUPPORTED_CLAIMS_FOUND`.

## Design-intent rules (enforced by the grounding gate and the compiler)

- **Root:** `"schema": "copper.design-intent"`, `"schema_version": 1`, `project` (`name`, `board.width_mm`,
  `board.height_mm`, `board.layers: 2`) and `components`. **No other keys anywhere.** Fields such as `mpn`, `rating`,
  `datasheet` or `routed` are rejected.
- **Components:** each has `reference` (R1, D1, J1 and so on), `value`, `symbol_id`, `footprint_id`, `pins` and,
  optionally, `position`.
- **Pins:** each has:
  - `symbol_pin`: the pin **number**;
  - `pin_name`: the library's name for that pin, exactly as in the context output;
  - `pad`: a real pad number;
  - `net`: a name, or `null` for an intentional no-connect.
- **Completeness:** every symbol pin and every numbered footprint pad appears exactly once.
- **Nets:** every named net connects at least two pins. A power-input pin may not be left `null`.
- **Placement:** give every component a `position` (`x_mm`, `y_mm`, `rotation_deg`). Each centre must be at least
  2.5 mm inside every board edge.
- **Unsupported symbols:** multi-unit symbols and derived symbols (for example `Regulator_Linear:AMS1117-3.3`) are
  rejected. Pick another standard part or ask the user.
- **No invented data:** no part numbers (MPNs), ratings, availability or prices. If a required part or pin mapping
  is uncertain, say so and ask.

## Honesty rules (these override everything above)

- **Results:** never say a design "passes", is "valid", "verified", "routed", "manufacturable" or "ready". The only
  results you report are the ones in `REPORT.md` and the `COPPER_RESULT` lines.
- **Separate results:** a GROUNDED gate is **not** a working circuit, a clean compile is **not** a clean ERC or DRC,
  and a clean DRC is **not** manufacturing approval. Every Copper design is **NOT APPROVED** for manufacturing.
- **UNVERIFIED and NOT RUN:** `UNVERIFIED` means not checked; it is never a pass. If a tool did not run, say
  "NOT RUN". Never estimate what it would have said.
- **KiCad version:** if `doctor` reports a KiCad other than 9.x, say that ERC and DRC were run with a development
  KiCad version and are not native evidence.
- **No manual fixes:** never hand-edit `.kicad_sch`, `.kicad_pcb`, `REPORT.md` or `copper_facts.json` to make a
  check pass. Fix the intent and compile again.

## Limits

- Work only inside this folder. Do not modify the Copper repository, run git, or install or upgrade software.
- Do not start servers other than `workbench`, and never expose them beyond this PC (no tunnels, no port forwarding).
- Board generation places footprints and assigns nets; it does **not** route tracks.
- `claimcheck` is a keyword check that catches common false claims. It cannot prove an answer correct, so
  `REPORT.md` stays the authority.
