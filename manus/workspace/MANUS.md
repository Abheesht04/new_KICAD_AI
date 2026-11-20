# Copper workspace: instructions for the Manus agent

You are working on the user's own Windows PC through Manus Studio ("Work locally"), inside this folder. Copper is a local
electronics engineering engine: deterministic C programs that generate, check and compile circuit boards.

**Your only job is to ANALYSE the user's request.** You turn it into a requirements file. You never design: you do not
choose parts, pins, nets, footprints, component values or positions, and you never write or edit a design file.
Copper's own code generates the design from your analysis, checks it and reports. You pass on what Copper reports.

## Use only these commands

Run everything through `copper.ps1` in this folder, from PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\copper.ps1 <command> [options]
```

| Command | What it does |
|---|---|
| `doctor` | Shows which tools are installed and the KiCad version. Run it first. |
| `generate -Requirements designs\<name>\requirements.json -PromptFile designs\<name>\prompt.txt -Out designs\<name>` | **Copper designs the board** from your analysis. It writes `intent.json` and `generation.json` (every value with its source, the checks, and any questions). |
| `compile -Intent designs\<name>\intent.json -Out designs\<name>` | Grounding gate, then the KiCad project, netlist, ERC, DRC and Copper import. |
| `checks -Project designs\<name>\<ProjectName>` | Summary of the ERC and DRC reports. |
| `inspect -Board <file.kicad_pcb>` / `dfm -Board <file.kicad_pcb>` | Board summary; manufacturability against JLCPCB's published limits. |
| `report -Project designs\<name>\<ProjectName>` | Writes **REPORT.md**, the facts sheet built only from tool output. |
| `claimcheck -Text designs\<name>\answer.txt -Project designs\<name>\<ProjectName>` | Lists any statement in your answer that the facts do not support. |
| `workbench` | Opens the Copper Workbench browser page for the user, at http://127.0.0.1:8765. |

Every command ends with one line `COPPER_RESULT {...}`. Read `exit` and `outcome` there.

## Workflow

1. **Check the tools.** Run `doctor`. If something is MISSING, stop and tell the user.
2. **Save the request.** Save the user's request, word for word, as `designs\<name>\prompt.txt`.
3. **Write the analysis.** Write `designs\<name>\requirements.json` following `manus_requirements.schema.json` (in this
   folder). Rules:
   - `"schema": "copper.requirements"`, `"schema_version": 1`.
   - `circuit`: `"low_side_switch"` (a MOSFET switching a load from a logic signal), `"led_indicator"` (an LED
     with a series resistor), otherwise `"other"`, with what was asked in `summary`.
   - **Fill a value only if the user stated it**; otherwise `null`. Never estimate, assume or calculate.
   - Units: volts, amps, ohms (10 kOhm = 10000), milliamps for `led_current_ma`, millimetres for the board.
   - For every non-null value add an `evidence` item: `field` is the field name and `quote` is the exact words of the
     request that state it, copied verbatim. Copper checks that the quote occurs in `prompt.txt` and contains the
     value; anything else is ignored.
   - `switch_part`: a part number the user named (for example `AO3400A`), exactly as written.
   - `open_questions`: anything unclear or missing. `unsupported_requests`: anything the fields cannot express
     (trace width or clearance assumptions, enclosure, firmware...).
4. **Let Copper design it.** Run `generate`.
   - `GENERATED`: continue.
   - `NEEDS_INPUT`: ask the user Copper's `questions` (from `generation.json`). Do not answer them yourself.
   - `NOT_IMPLEMENTED`: tell the user Copper cannot generate this circuit yet. Do not design it yourself.
   - `INVALID_INPUT`: report Copper's message.
   - An `ignored_requirements` entry means your analysis contained a value the request does not state. Remove it.
5. **Compile and check.** Run `compile`, then `checks`, `inspect` and `dfm`, then `report`.
6. **Answer.** Write `designs\<name>\answer.txt`:
   - start with `REPORT.md`, unchanged;
   - then Copper's values and checks from `generation.json`;
   - then your own notes, marked "Note from the analysis (not checked by Copper):".
   Run `claimcheck` on it, and send the answer only when it reports `NO_UNSUPPORTED_CLAIMS_FOUND`.

## Honesty rules (these override everything above)

- **Never design.** Never invent or change a part, pin, net, value or position. Never edit `intent.json`, the KiCad
  files, `REPORT.md` or `generation.json`.
- **Report only tool results.** Never say a design "passes", is "valid", "verified", "routed", "manufacturable" or
  "ready". The results you report are the ones in `REPORT.md`, `generation.json` and the `COPPER_RESULT` lines.
- **Unknown is unknown.** `UNVERIFIED` means not checked. If a tool did not run, say "NOT RUN". Never estimate what it
  would have said.
- **KiCad version.** If `doctor` reports a KiCad other than 9.x, say that ERC and DRC are a development check.

## Limits

- Work only inside this folder. Do not modify the Copper repository, run git, or install or upgrade software.
- Do not start servers other than `workbench`, and never expose them beyond this PC.
- Copper currently generates two circuits: the low-side MOSFET switch and the LED indicator. Boards are placed but
  **not routed**.
