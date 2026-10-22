# pcba command-line interface

`pcba` is the single user-facing PCBAI command. It owns the command tree, argument
validation, help, colour, the JSON envelope and the exit codes. It does no engineering
work itself:

```text
pcba (apps/pcba, C)                      front door: parse, validate, report
  └─ tools/copper.py --result-json FILE  orchestration (libraries, routing, export, KiCad gates)
       └─ copper-core (apps/copper_core.c → hwcore)   physics, exact geometry, DFM
```

Process launching lives in `apps/pcba/cli_os.c` (target `cli_support`), which `hwc` also
uses. It passes an argv array straight to `CreateProcessW`/`execvp`; no shell and no joined
command string is involved, so paths with spaces, quotes, `%VAR%`, `&`, `;` or `$(...)`
reach the child unchanged.

## Commands

| Command | Status | Backend |
|---|---|---|
| `pcba help [command]` | stable | — |
| `pcba version` | stable | — |
| `pcba doctor` | stable | pcba runtime checks + `copper.py doctor` |
| `pcba test [--quick\|--full]` | stable | CTest of the build tree containing pcba |
| `pcba project new <template> --out FILE` | stable | `copper.py new` |
| `pcba project check <file.copper>` | stable | `copper.py check` |
| `pcba project build <file.copper> --out DIR` | stable | `copper.py build` |
| `pcba project verify --out DIR` | stable | `copper.py verify` |
| `pcba project fabricate --out DIR` | stable | `copper.py fabricate` |
| `pcba simulate dc <file.copper>` | stable | `copper.py check --scope dc` |
| `pcba simulate ac <file.copper> --start HZ --stop HZ --points N` | stable | `copper.py ac` → `copper-core ac` |
| `pcba simulate transient <file.copper> --dt S --steps N [--method be\|trap]` | stable | `copper.py transient` |
| `pcba parts search <query>` | stable | `copper.py catalogue` |
| `pcba parts add --catalogue FILE --id ID --kind K` | stable | `copper.py part-add` |
| `pcba parts remove --catalogue FILE --id ID` | stable | `copper.py part-remove` |
| `pcba library scan` | stable | `copper.py library-scan` (reuses `hwc_kicad.py` indexers) |
| `pcba dfm check <file-or-build-dir>` | stable | `copper.py dfm` |
| `pcba spec parse ["text"] [--file F] [--out F.espec]` | stable | `copper.py spec-parse` (`tools/spec_nlp.py`) |
| `pcba spec check <file.espec>` | stable | `copper.py spec-check` (`tools/spec_ir.py`) |
| `pcba spec lower <file.espec> --out F.copper` | stable | `copper.py spec-lower` → loaded by `copper-core analyze` before publishing |
| `pcba parts import <catalogue-file> [--into FILE]` | stable | `copper.py part-import` (copper-core schema + KiCad library parity; docs/engineering/PART_REGISTRY.md) |
| `pcba design "request" --out DIR [--profile F] [--answers K=V,...]` | stable | `copper.py design` → `tools/design.py` (docs/engineering/DESIGN.md) |
| `pcba place <file.copper> --out FILE` | stable | `copper.py place` → `tools/layout.py` (docs/engineering/LAYOUT.md) |
| `pcba route <file.copper> --out DIR` | stable | `copper.py route` → `tools/layout.py` |

Every command is implemented. The NOT_IMPLEMENTED mechanism remains: a planned command is listed
separately in `pcba --help`, validates its arguments, and exits 4 without doing anything.

## Mapping from earlier entry points

| Earlier | Now | Notes |
|---|---|---|
| `copper.py new/check/build/verify/fabricate` | `pcba project …` | copper.py keeps its own CLI and 0/2 exit codes for compatibility |
| `copper.py transient … --out` | `pcba simulate transient` | `--out` is now optional in both (CSV on stdout) |
| `copper.py catalogue --search` | `pcba parts search` | |
| `copper.py part-add` | `pcba parts add` | `--file` is `--catalogue` in pcba |
| `copper.py classify` | not exposed | old language classifier; the charter forbids building V2 NLP on it. Still in `copper-core classify`. The V2 frontend is `pcba spec parse` (docs/engineering/ENGINEERING_SPEC.md), a new implementation |
| `hwc_kicad.py catalog` + `footprints` | `pcba library scan` | same JSON/CSV format |
| `hwc_kicad.py doctor` | `pcba doctor` | |
| `hwc_kicad.py search/search-footprint/generate/validate/custom-symbol/name` | not exposed | V1 helpers; still runnable directly |
| `hwc make/validate/DFM/physics-apply/catalogue/kicad-check/status` | not exposed | legacy V1 `Design` path; `hwc` is kept as a deprecated compatibility CLI |
| `hwc project …` | `pcba project …` | still forwards to copper.py (now via `cli_run`, fixing paths with spaces) |
| `copper-core analyze/transient/classify/models/catalogue` | internal backend | machine-readable; `models` lists device model specs, `catalogue FILE` validates a catalogue |
| `fabcheck_demo` | developer tool | unchanged |
| `apps/prompt.c` | none | not built by any target (needs `Extras/NLP`); left for Prompt 6 reference analysis |

## Exit codes and statuses

| Exit | Status | Meaning |
|---|---|---|
| 0 | PASS | the check ran and passed |
| 1 | FAIL | the check ran and failed |
| 2 | INVALID_INPUT | an input file or value violates its contract (including missing input files) |
| 3 | UNVERIFIED | a required tool, model, data or geometry was unavailable |
| 4 | NOT_IMPLEMENTED | the capability does not exist yet |
| 64 | USAGE | invalid command-line usage (unknown command/option, missing or malformed argument) |
| 70 | ERROR | internal or environment error; nothing was established |

What each command's status covers:

- `project check`: worst of electrical and physical/DFM scope. A source project without board
  geometry, or with unknown ratings or heights, is UNVERIFIED.
- `simulate dc`: the DC solve only. PASS when solved; FAIL when singular or not converged;
  UNVERIFIED when a device has no model.
- `project build`: internal checks only. The result always says `native_verification: NOT_RUN`.
- `project verify`: PASS only when hashes, source freshness, re-analysis, data completeness and
  every KiCad 9.x gate ran and passed. A missing KiCad, or a non-9.x version, is UNVERIFIED.
- `dfm check`: physical scope. Without geometry, or without a DC solution (no current budgets),
  it is UNVERIFIED.
- `doctor`: FAIL if a core requirement is broken, UNVERIFIED if KiCad 9.x or its libraries are
  missing. Desktop checks never affect the status.
- `test`: FAIL if CTest fails, UNVERIFIED if any suite was skipped.

pcba never reports success when the backend is inconsistent. A missing or malformed result
file, or `PASS` with a non-zero backend exit, becomes `ERROR` (70).

## Output

- Human mode writes results to stdout and ends with one status line,
  `STATUS command: message`. The status word is always printed, so colour is never needed
  to read it. Usage and internal errors go to stderr.
- `--json` writes exactly one document to stdout, for every command and every error
  (including usage errors):

  ```json
  {"schema":1,"tool":"pcba","version":"2.0.0-dev","command":"project check",
   "status":"UNVERIFIED","exit_code":3,"message":"","result":{...}}
  ```

  For backend commands, `result` is the backend's `{"status","message","data"}` object.
  Child output and progress are redirected to stderr.
- Colour is used only when stdout is a terminal with ANSI support. It is disabled by
  `--no-color`, `NO_COLOR`, `TERM=dumb` or `--json`. `PCBA_FORCE_COLOR=1` forces it on, for
  tests; `NO_COLOR` and `--no-color` still win.

## Backend protocol (`tools/copper.py`)

`copper.py --format human|quiet|json --result-json FILE <command> …` writes
`{"status": …, "message": …, "data": …}`, with status as the first key, on every handled exit
path. Exception classes map to statuses: `Unverified`/`ToolUnavailable` → UNVERIFIED,
`Failed`/`CalledProcessError` → FAIL, `CoreError` → ERROR, any other `ValueError` →
INVALID_INPUT. An unhandled crash writes nothing, which pcba reports as ERROR. Unsupported
KiCad geometry (multi-unit symbols, zones, slots, other pad layers, non-90° rotations) is
UNVERIFIED, not invalid input.

## Environment

| Variable | Used by | Effect |
|---|---|---|
| `PCBA_PYTHON` | pcba | Python executable (honoured strictly). Otherwise `HWC_PYTHON`, then `py -3`/`python`/`python3` |
| `PCBA_ROOT` | pcba, hwc | source tree holding `tools/`. Default: the configured source directory |
| `COPPER_CORE` | copper.py | copper-core path. pcba and hwc set it to their sibling build when unset |
| `KICAD_CLI` | copper.py | kicad-cli path. Discovery prefers KiCad 9.0 installs |
| `KICAD_SYMBOL_DIR`, `KICAD_FOOTPRINT_DIR` | copper.py | KiCad library roots |
| `NO_COLOR`, `PCBA_FORCE_COLOR` | pcba | colour policy above |

## Tests

- `tests/test_pcba_cli.py` (CTest `pcba_cli`) covers:
  - help and version;
  - every invalid or missing argument (exit 64, nothing on stdout, a JSON envelope with `--json`);
  - missing inputs (exit 2) and planned commands (exit 4);
  - the colour policy, and JSON purity with forced colour;
  - status propagation from the solver and backend;
  - verification that can never succeed when it is stale, tampered or missing KiCad;
  - a fake backend that crashes, lies or writes garbage;
  - paths with spaces, `&;`, `%PATH%`, quotes and non-ASCII, through both `pcba` and `hwc`;
  - verbatim argument transport;
  - a source scan for shell use;
  - legacy `hwc`/`copper-core`;
  - doctor.
- `tests/cli_quote_test.c` (CTest `cli_quote`) checks the Windows quoting rules against
  hand-written expectations.
