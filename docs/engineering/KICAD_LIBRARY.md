# KiCad environment and library index

`tools/kicad_library.py` is the single owner of finding KiCad. `hwc_kicad.discover_*` (used by build, design,
parts import and doctor) asks it for installs instead of keeping its own list of folders.

## Installs and policy

`pcba library env` finds every install:

- **Where it looks:** `Program Files\KiCad\*` and `%LOCALAPPDATA%\Programs\KiCad\*`, `/usr`, `/usr/local`, `/Applications/KiCad/KiCad.app`, the install that holds `kicad-cli` on `PATH`, and `KICAD_CLI`.
- **What it records:** the root, the `kicad-cli` path and the library folders.
- **How it reads the version:** from `kicad-cli version`. A folder name like `9.0` is used only when the CLI cannot be run, and the source of the version is recorded.

**Policy:** the selected install is the highest **9.x** that has `kicad-cli` and both library folders. Ties go to a version read from the CLI, then to the root path. No other major version is ever selected. With only KiCad 10 installed, the snapshot lists it and the result is UNVERIFIED.

Library lookups that do not need native validation try 9.x first, then the other versions, newest first.

## Library tables and variables

For symbols and footprints separately, KiCad's tables are read in this order:

1. **The global table.** This is the user's `sym-lib-table` / `fp-lib-table` in the per-major settings folder: `%APPDATA%\kicad\9.0`, `~/.config/kicad/9.0` or `~/Library/Preferences/kicad/9.0`, or `$KICAD_CONFIG_HOME/9.0`. Before KiCad's first run, the install's template table is used instead (KiCad copies it on first run).
2. **The project table**, when `--project DIR` is given. Its nicknames override global ones.

`${VAR}` and `$(VAR)` resolve in KiCad's order:

1. the process environment;
2. `environment.vars` in `kicad_common.json`;
3. the install's own `KICAD9_SYMBOL_DIR`, `KICAD9_FOOTPRINT_DIR`, `KICAD9_3DMODEL_DIR` and `KICAD9_TEMPLATE_DIR`.

`KIPRJMOD` is the project folder. The snapshot records each variable with its source.

Every table entry keeps one status:

| Status | Meaning |
|---|---|
| `ok` | the file (symbols) or `.pretty` folder (footprints) exists |
| `disabled` | `(disabled)` in the table |
| `missing` | expanded path does not exist |
| `unresolved` | a `${VAR}` has no definition (listed in `unresolved`) |
| `unsupported_type` | not a KiCad-format library (Eagle, Database, nested tables, ...); not indexed |
| `duplicate` | a second entry with the same nickname in one table; KiCad uses the first |
| `overridden` | a global entry replaced by the project entry with the same nickname |

Only `ok` entries are indexed.

## The index

`pcba library scan` writes `out/library/index.sqlite` and `out/library/environment.json`.

**Where the libraries come from:** `--symbols` / `--footprints`, or `KICAD_SYMBOL_DIR` / `KICAD_FOOTPRINT_DIR`, index that folder instead of the tables.

**What is stored:**

| Table | Fields |
|---|---|
| `symbols` | id `LIB:NAME`, extends, Reference, Value, Description (or `ki_description`), `ki_keywords`, `ki_fp_filters`, default Footprint, power flag, unit count, pins (number, name, electrical type, unit), the pin-number set |
| `footprints` | id, description, tags, `attr` flags, pads (number, type), the pad-number set, courtyard bounding box (F/B.CrtYd lines, rectangles, circles, arcs, polygons; null when there is none), 3D model count |
| `symbol_text`, `footprint_text` | FTS5 full-text over name, description and keywords or tags |
| `sources` | per library: path, file-change signature, file format version, read error |

A derived symbol takes its parent's pins and properties, and its own properties win. A pad with no number, or of type `np_thru_hole`, is a mechanical hole, not a pad.

**It is a cache, never hand-maintained:**
- Each library has a signature built from the size and modification time of its files. Only new or changed libraries are parsed again, and removed ones are dropped.
- If the stored schema differs from the code's, the index is rebuilt.
- Libraries are parsed in worker processes, largest first, and written in a fixed order. The database is therefore byte-identical whatever `--jobs` is (tested).

**A library that cannot be read** is listed with its error, gets no rows, and makes the scan UNVERIFIED.

**How it reads files:** the index needs a few fields from very large files (a 16 MB MCU library is 4 million tokens), so it does not build the whole tree:
- Quoted strings and comments are blanked first, keeping every offset, so the parentheses that remain are structural.
- The depth of each node of interest comes from C-level counts between nodes.
- Only small nodes (courtyard graphics, attributes) go through the strict parser.

`tests/test_kicad_library.py` compares every field with a full-tree reading:
- on the committed KiCad 9 snapshot;
- on synthetic edge cases: parentheses and escaped quotes inside strings, comments, derived symbols, units, body styles, alternates, power symbols, mechanical pads, circle and polygon courtyards;
- with `KICAD_SYMBOL_DIR` / `KICAD_FOOTPRINT_DIR` set, on every library there. On 2026-10-26 that was all 378 libraries of a KiCad 10.0.5 copy: 22,784 symbols and 15,447 footprints, identical.

The index is for search and pairing. Builds still read each symbol and footprint they use with the strict parser.

## Search and pairing

**`pcba library search QUERY [--kind symbol|footprint]`:**
- Every word must match the name, description or keywords/tags as a prefix.
- Results are ranked by FTS5 bm25 (name weighted highest), then by id.
- No match is PASS with an empty list, as in `parts search`.

**`pcba library pair LIB:SYMBOL`:**
- Lists the footprints whose pad-number set **equals** the symbol's pin-number set.
- Ranking: the symbol's default footprint first, then footprints its `ki_fp_filters` accept (case-insensitive wildcards on the name, or on `LIB:NAME` when the filter contains `:`), then the rest, by id within each group.
- Filters rank; they never add a footprint whose pads differ.
- The result also says whether the default footprint's pads match.
- UNVERIFIED when nothing pairs; INVALID_INPUT for an unknown symbol.

## Measured (2026-10-26, 4 logical cores, Python 3.12)

Development data: a KiCad 10.0.5 library copy, 234 MB of symbols and 194 MB of footprints.

| Run | Time |
|---|---|
| Cold index, first version (full-tree parse, per-row inserts) | 292 s |
| Cold index, selective reader + bulk inserts, `--jobs 4` | **33.8 s** (single process: about 50 s) |
| Warm re-scan, nothing changed | **0.9 s** |
| Search | 5-25 ms |

The largest symbol library (`MCU_ST_STM32H7`, 16 MB) takes 1.9 s instead of 7 s. In the remaining cold time the main process is waiting on the parsers.

## Not done yet

- `copper.resolve_symbol` and part import still read the library folders directly; they do not use the index yet.
- Nested library tables, database libraries and other formats are listed as `unsupported_type`, not read.
- 3D model files are counted, not checked.
