# Copper Lab — local prototype

Copper Lab is a small local web UI around the existing Copper Release executable, KiCad CLI 9 and Ollama. It is intentionally not a hosted website or a general shell interface.

## What it does

1. Sends the written engineering request to the selected local Ollama model (default `qwen3:8b`). The model is asked to return a KiCad 9 schematic and matching PCB as complete S-expressions, plus requirements, assumptions and unresolved questions.
2. Writes the raw prompt through Copper's existing `user design prompt` command, preserving `copper_request.draft.json`.
3. Performs a basic lexical parenthesis/string check before promoting model output to `.kicad_sch` and `.kicad_pcb` files.
4. When KiCad CLI is available, runs schematic ERC, schematic netlist export, PCB DRC, and SVG previews.
5. When Copper Release is available, runs design import (design/diagnostic/manifest JSON), topology analysis when a netlist was generated, board inspection, structural validation, and Copper DFM.
6. Shows source previews or KiCad-rendered SVG previews, check outputs, and links to all files in the browser.

**Important implementation boundary:** the C/Copper source provided in the uploaded archive has no prompt-to-KiCad file generator. It has a prompt-capture command and analysis/validation commands. The first generation step is therefore done by Qwen through Ollama; Copper and KiCad validate the resulting files. This is an experimental generation path, not a claim that Qwen-generated output is valid. Failed candidates and logs are retained for inspection.

## Requirements

- Windows 10/11
- Python 3.10+ (the web wrapper uses only Python's standard library)
- Ollama running locally, with `qwen3:8b` pulled
- Copper Release executable at `build\Release\copper.exe` (or set `COPPER_EXE`)
- KiCad 9 CLI at `C:\Program Files\KiCad\9.0\bin\kicad-cli.exe` (or set `KICAD_CLI`)

Your reported local versions (`ollama 0.40.16`, `qwen3:8b`, `KiCad 9.0.6`) are the expected setup. The wrapper does not pull models automatically.

## Start

From the project root in PowerShell 5.1:

```powershell
.\run_copper_lab.ps1
```

Open [http://127.0.0.1:8765](http://127.0.0.1:8765). Keep the PowerShell window open. Press Ctrl+C there to stop the server.

If paths are different:

```powershell
.\run_copper_lab.ps1 `
  -CopperExe 'D:\path\to\copper.exe' `
  -KiCadCli 'C:\Program Files\KiCad\9.0\bin\kicad-cli.exe' `
  -Model 'qwen3:8b'
```

Or set environment variables directly (`COPPER_EXE`, `KICAD_CLI`, `OLLAMA_HOST`, `COPPER_OLLAMA_MODEL`) and run:

```powershell
python .\copper_lab\server.py
```

## Output location

Each run is saved under:

```text
copper_lab\runs\<run-id>\
```

The run includes the prompt capture, Qwen JSON, model candidate text, KiCad project files, netlist, native DRC/ERC JSON and reports, preview SVGs, Copper JSON/DFM outputs, logs, and `run_manifest.json`.

## Local-only safety

- The server refuses to bind to a non-loopback address.
- Ollama stays at `127.0.0.1:11434` by default.
- No shell command is accepted from the browser. Only fixed Copper/KiCad command templates are run.
- The browser can read artifacts only from Copper Lab's run folders.
- Do not tunnel or port-forward this prototype to the public internet. Do not expose Ollama's API or local command execution.

## What is not implemented yet

- Automatic correction loops that edit and rerun a rejected design.
- Reliable component/library resolution for arbitrary symbols and footprints.
- A Copper-owned deterministic KiCad writer. The model authors the initial project files in this prototype.
- Electrical simulation, thermal analysis, flight qualification, radiation analysis, or spacecraft verification.
- Manufacturing release/qualification. Passing DRC/ERC/DFM checks is not manufacturing or mission approval.

A good first test is a simple low-voltage indicator board with a known supply, an LED, and a resistor. Review the generated assumptions and unresolved questions before judging the toolchain.
