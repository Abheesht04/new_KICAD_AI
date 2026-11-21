# Using Copper with Manus

[Manus](https://manus.im) is an AI agent that runs in the cloud. To use Copper, which runs on your own PC, Manus needs
its desktop app, **Manus Studio**, and the **"My Computer" / "Work locally"** feature. With it, the agent works inside
one folder you choose and runs local tools there ([Manus docs: desktop app](https://manus.im/docs/features/desktop.md)).
This kit sets up that folder.

Why not a Manus "Custom MCP server" instead? Manus connects to custom MCP servers by an HTTPS URL that its cloud must
reach ([Manus docs: custom MCP](https://manus.im/docs/integrations/custom-mcp.md)). Copper's server runs only on this
PC, has no login and can export files, so publishing it to the internet would be unsafe. Studio's local mode keeps
everything on your PC.

## How it works

```
You ──task──> Manus agent (in Manus Studio, "Work locally" on D:\copper-manus)
                │ writes design-intent JSON (Manus is the only design author; Copper runs no model)
                ▼
          copper.ps1 ──> Copper design compiler ──> KiCad project ──> KiCad netlist, ERC, DRC ──> Copper import
                    └──> Copper board-inspect, DFM (JLCPCB limits), topology
                │ every step prints COPPER_RESULT {...}
                ▼
          Manus reports what Copper and KiCad said; it decides nothing itself (MANUS.md)
```

## Set up (once)

1. Build Copper (see the main README), so that `build\copper.exe` and `build\copper-core.exe` exist.
2. Run, from the repository root:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\manus\setup_manus_workspace.ps1
   ```
   - It creates `D:\copper-manus` with `MANUS.md` (the agent's instructions), `copper.ps1` (its only command), the
     design-intent schema and an example. Change the folder with `-Workspace`.
   - KiCad 9 is found at `C:\Program Files\KiCad\9.0`. Otherwise pass `-KiCadCli <kicad-cli.exe>` and
     `-KiCadShare <folder with symbols\ and footprints\>`.
3. Check everything: `powershell -ExecutionPolicy Bypass -File .\manus\selftest.ps1`.
4. Install **Manus Studio** for Windows from https://manus.im/desktop and sign in. It needs Windows 10/11 and a Manus
   account; this step is yours.
5. In Manus Studio, turn on **Work locally**, then **Choose a folder**: `D:\copper-manus`.
6. Under **Permissions**, use **Ask for approval**. You then see each command before it runs.

## Use

Start each task with: **"Read MANUS.md in this folder first and follow it."** Then, for example:

- "Design a 5 V LED indicator board: a 2-pin header, a 1 k resistor and a red 0603 LED. Compile it and report every
  ERC, DRC and DFM finding."
- "Inspect D:\work\myboard.kicad_pcb and check it against JLCPCB's limits."
- "Open the Copper Workbench for me."

## What to expect

- **Routed by Copper's C router.** `compile` routes the placed board on two layers (`copper design route`,
  `src/v2_src/autoroute.c`) with the rules in `routing_profiles/copper_2layer_default.json`, which it first checks
  against the fab profile. Each net's copper is checked with exact integer geometry before it is written; a net that
  fails is left out and reported, and the facts sheet says **ROUTED** only when the router routed every net and
  KiCad's DRC lists 0 unconnected items. Track widths are template choices, not computed from current.
- **Read all three results.** Compile success, clean ERC/DRC and DFM are separate results. The agent is told to
  report each one and never to call a board manufacturing-ready.
- **KiCad version.** With a KiCad other than 9.x, ERC and DRC results are a development check, and `doctor` says so.
- **Manus is the only design author.** Copper and Copper Lab run no language model; a design intent Manus writes can
  also be pasted into Copper Lab (`run_copper_lab.ps1`, http://127.0.0.1:8780 with `-Port 8780`) to compile it there.

## Manus API key (optional)

Manus Studio signs in with your Manus account and does **not** need an API key. A key is only needed if Copper itself
should call the Manus API (`https://api.manus.ai`, header `x-manus-api-key`), for example to send a design task from
Copper Lab.

```powershell
powershell -ExecutionPolicy Bypass -File .\manus\set_manus_api_key.ps1            # asks for the key; nothing is shown
powershell -ExecutionPolicy Bypass -File .\manus\set_manus_api_key.ps1 -Status    # is a key stored?
powershell -ExecutionPolicy Bypass -File .\manus\set_manus_api_key.ps1 -Remove    # delete it
```

- **Where it is kept:** `%APPDATA%\Copper\manus_api_key.dpapi`, encrypted with Windows DPAPI for your Windows account
  only, and outside the repository.
- **Never put the key** in a chat, a commit, `copper.config.json` or an environment variable shared with other tools.
