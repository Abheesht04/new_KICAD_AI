# Copper Lab: local design compiler page

Copper Lab is a small web page on your own PC. It runs the Copper design compiler, KiCad CLI and the Copper CLI. It
runs **no language model**: designs come from **Manus**. The Manus agent writes a `copper.design-intent` JSON in its
workspace (see [manus/README.md](../../../manus/README.md)), and you can compile that intent here to see the
schematic and PCB previews, every KiCad check and all the files.

The Qwen/Ollama generator that came with the `feature/copper-lab-integrated` branch was removed on 2026-11-20 at
the owner's request; Manus is the only design author.

## What it does

1. **Records the request (optional).** The request text is saved through Copper's `user design prompt` command.
2. **Compiles the design intent.** `src/v2_src/tools/copper_design_compile.py` writes a native KiCad project
   (`.kicad_sch`, `.kicad_pcb`, `.kicad_pro`). It uses the symbols and footprints installed on this PC, with their
   real pin and pad numbers.
3. **Runs the KiCad CLI.** Netlist export, ERC and DRC, plus SVG previews.
   - ERC and DRC are judged from KiCad's own reports, not its exit code (kicad-cli exits 0 even when it finds
     violations). A check is "passed" only when its report is empty; otherwise it shows "findings", with counts by
     severity and by type.
4. **Runs the Copper import** of the generated project.
5. **Keeps everything.** All files and a `run_manifest.json` are saved under `copper_lab\runs\<run-id>\`, which git
   ignores. Every run is marked **NOT APPROVED**.

## Start

```powershell
.\run_copper_lab.ps1 -Port 8780
```

Open http://127.0.0.1:8780. Port 8765 is used by the Copper Workbench.

The launcher finds `build\copper.exe` and KiCad 9 at `C:\Program Files\KiCad\9.0`. Otherwise use `-KiCadCli <path>`
and set `COPPER_SYMBOLS_ROOT` and `COPPER_FOOTPRINTS_ROOT`.

## Use

- **In the page:** paste the intent JSON Manus wrote (for example `D:\copper-manus\designs\<name>\intent.json`), add
  the request text if you like, and click **Compile & validate**. **Load the LED example intent** fills in a
  working example.
- **Over the API:** `POST /api/compile` with `Content-Type: application/json` and the body
  `{"intent": {...}, "prompt": "..."}`.
- **Pin and pad lookup:** `POST /api/context` with `{"text": "symbol Device:R footprint Resistor_SMD:R_0603_1608Metric"}`
  returns the real pin and pad numbers for the named parts.

## Local-only safety

- The server binds only to `127.0.0.1`.
- A POST must be `application/json`, so a page on another site cannot drive it through your browser.
- No shell command is accepted. Only fixed Copper and KiCad commands run, and files are served only from the run
  folders.

## Limits

- Two-layer boards, single-sheet and single-unit symbols, and explicit pin-to-pad mapping.
- Derived and multi-unit symbols are rejected.
- Parts are placed but **not routed**: DRC lists the unconnected nets.
- ERC, DRC and DFM results are findings for review, not approval.
