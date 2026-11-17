# Copper local-PC validation (Windows PowerShell 5.1)

Run from the Copper repository root after building `build\Release\copper.exe`.

## 1. KiCad CLI by absolute path

```powershell
$kicad = 'C:\Program Files\KiCad\9.0\bin\kicad-cli.exe'
Test-Path -LiteralPath $kicad
& $kicad version
```

If installed elsewhere, change `$kicad` to the actual `kicad-cli.exe` path.

## 2. Run the local validation suite

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\v2_src\tests\local_pc\run_local_validation.ps1 `
  -CopperExe .\build\Release\copper.exe `
  -KiCadCli $kicad `
  -SampleRoot 'C:\Program Files\KiCad\9.0\share\kicad\demos' `
  -OutputRoot .\validation\local_pc
```

The script creates a timestamped run folder with build-independent CLI smoke tests,
KiCad schematic/netlist tests, board corpus results, JSON outputs and logs.

## 3. Ollama smoke test

First install/start Ollama. Then, if needed:

```powershell
ollama pull qwen3:8b
```

Run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\v2_src\tests\local_pc\ollama_smoke_test.ps1 `
  -Model qwen3:8b `
  -OutputRoot .\validation\local_pc\ollama
```

The smoke test checks the local HTTP API and asks the model to return a structured
requirements analysis. It does not grant the model shell access and does not connect
it to Copper mutation tools. Tool-host integration is a separate, explicitly gated step.
