# Copper Lab — Structured Design Compiler Integration

This is an overlay patch for the current Copper Lab project. It replaces the old website generation path (Qwen writing complete KiCad S-expressions) with the JSON-first Copper Design Compiler pipeline.

## What changed

1. The website `/api/generate` now creates the Copper request JSON using the existing `build\Release\copper.exe`.
2. It reads actual local KiCad 9 symbol pin and footprint pad metadata. When the prompt doesn't name exact library IDs, it supplies a small candidate set and filters it against installed library files.
3. Ollama `/api/chat` is called with the `copper.design-intent.v1` JSON Schema in its `format` field.
4. The current Python 3.12 interpreter invokes `v2_src/tools/copper_design_compile.py`.
5. The compiler calls the existing Copper Release CLI for component resolution and project import, and KiCad CLI for netlist/ERC/DRC.
6. The website receives recursive artifact paths and can preview/download nested validation and `.copper` files.
7. The launcher explicitly resolves Python 3.12 and prints the selected interpreter.

Ollama documents the `format` parameter as accepting a JSON Schema object for structured chat responses: https://github.com/ollama/ollama/blob/main/docs/api.md#structured-outputs

## Apply

1. Stop Copper Lab with `Ctrl+C`.
2. Extract this ZIP into the folder containing `run_copper_lab.ps1`, merging the included folders. Do not replace `build` or the main C source.
3. Keep your existing `build\Release\copper.exe` and KiCad 9 installation. No CMake rebuild is required for this integration patch.
4. Run: `\.\run_copper_lab.ps1 -Model qwen3:8b -Context 4096`
5. Hard-refresh the browser with `Ctrl+Shift+R`.
6. Use the updated example request and click Generate.

The server should print `[Copper Lab] POST /api/generate started` immediately, then perform request capture, structured Ollama generation, library validation, compiler invocation and native validation. The final POST access log appears after the response completes because this prototype uses a synchronous request handler.

## Python

Default interpreter: `%LOCALAPPDATA%\Programs\Python\Python312\python.exe`. Override with `-Python C:\path\to\python.exe`. The compiler is invoked via `sys.executable`, so it uses the same interpreter as the server.

## Test

Run `scripts\test_copper_lab_pipeline.ps1`. It checks Python syntax, web/bridge tests, and compiler tests. It also uses the existing C CLI for the compiler import integration test.

## Current scope and limitations

- Two-layer board generation only.
- Single-sheet, single-unit symbol generation only.
- Requires explicit symbol-pin to footprint-pad mapping.
- Compiler v1 rejects derived symbols (including the stock KiCad `Regulator_Linear:AMS1117-3.3` inheritance form) and multi-unit symbols. Such requests stop before Qwen is asked to substitute a different part.
- Uses installed KiCad libraries; the model cannot invent library assets. Explicitly requested assets that fail local inspection are rejected rather than silently substituted.
- The initial PCB gets actual footprints and pad net assignments but is **not autorouted**: no traces or vias are generated yet.
- ERC/DRC outputs are findings/evidence, not design approval. Human engineering review remains mandatory.

## Runtime configuration

- `COPPER_EXE`: override Copper CLI executable.
- `KICAD_CLI`: override KiCad CLI executable.
- `COPPER_SYMBOLS_ROOT`: override KiCad symbols directory.
- `COPPER_FOOTPRINTS_ROOT`: override KiCad footprints directory.
- `COPPER_OLLAMA_CONTEXT`: context size; default 4096.
- `OLLAMA_HOST`: default `http://127.0.0.1:11434`.
