# Tool server (V2 track F)

`src/v2_src/tool_server.{c,h}` is a JSON-RPC 2.0 server over line-delimited stdio: one JSON message per line in, one compact response per line out. Notifications get no response.

The library does no I/O; `copper-core serve` runs the loop:

```
copper-core serve [--root DIR] [--kicad-cli PATH] [--log FILE]
```

| Option | Effect |
|---|---|
| `--root DIR` | relative paths in requests resolve against DIR; replies echo paths as given |
| `--kicad-cli PATH` | fixes the CLI used by `export.fab`. Otherwise it is found through `KICAD_CLI_PATH`, `PATH` or the KiCad 9 install folders. |
| `--log FILE` | appends the session log (JSON lines) |

The loop ends at end of input, or after `exit`.

## Methods

| Method | Result |
|---|---|
| `initialize` | `protocolVersion` `copper.tools/1`, `serverInfo`, `capabilities` |
| `tools/list` | every tool with its description and JSON `inputSchema` |
| `tools/call {name, arguments}` | the tool's result. Any tool can also be called directly as a method, with its arguments as `params`. |
| `shutdown` | `null`; after it, every request except `exit` is refused |
| `exit` (notification) | stops the loop |

**Tools.** Arguments are checked against the schema: an unknown argument is an error, never ignored.

| Tool | Arguments | Does |
|---|---|---|
| `board.load` | `path` | Loads a `.kicad_pcb` into the board model (track B). Returns `board_id`, counts, the outline (closure, gap, bounds) and a findings report of model issues. |
| `dfm.check` | `board_id` or `path`, `profile` | DFM against a fab profile (track C): a findings report (track E) with each limit's source quote. |
| `excellon.check` | `path`, optional `board_id` or `board`, `tolerance_nm` (default 500) | Reads an Excellon file. With a board, every drilled hole must be a board hole, at the same position (Excellon y = −board y) within the tolerance and with the same diameter, and every round board hole must be drilled. The result is `cam/hole_missing`, `cam/hole_extra` and `cam/diameter_mismatch` FAILs; slots and blind vias are counted, not compared. |
| `export.fab` | `board_id` or `path`, `out` | Gerbers and drill files through kicad-cli (track D) into a new folder `out` (`out/gerbers`, `out/drill`; never over anything). Every file is read back (tracks A and E) and the holes are compared with the board as above. Reports which kicad-cli version ran and whether it is 9.x. |

## Errors

Errors are JSON-RPC error objects. `data` always carries a machine-readable `kind` and usually a `hint`, plus `file`, `line`, `item`, `key` and `detail` where they are known.

| Code | Meaning | Example `kind` |
|---|---|---|
| −32700 | the line is not JSON | `PARSE_ERROR` (with the column) |
| −32600 | not a JSON-RPC 2.0 request; batches; a refusal after shutdown | `INVALID_REQUEST`, `BATCH_UNSUPPORTED`, `SHUT_DOWN` |
| −32601 | unknown method or tool | `UNKNOWN_METHOD`, `UNKNOWN_TOOL` |
| −32602 | invalid params | `UNKNOWN_ARGUMENT`, `MISSING_PATH`, `BAD_TOLERANCE` |
| −32001 | input not found | `BOARD_NOT_FOUND`, `PROFILE_NOT_FOUND`, `DRILL_NOT_FOUND` |
| −32002 | input invalid | the board or profile status, with line, item or key |
| −32003 | unknown `board_id` | `UNKNOWN_BOARD_ID` |
| −32010 | kicad-cli unavailable | `KICAD_CLI_NOT_FOUND` |
| −32011 | kicad-cli failed | the runner status, with stderr in `detail` |
| −32012 | output exists | `OUTPUT_EXISTS` |

**Session log:** one JSON line per message, in the form `{"seq":n,"in":<request>}` and `{"seq":n,"out":<response>}`. A request that is not JSON is logged as a string. The log carries no timestamps, so it is reproducible.

## Evidence

**`tool_server` test:** `tests/golden/tool_server/requests.jsonl` is a 25-message script, run with root = the repository and kicad-cli = the track D fake. The responses must equal `responses.jsonl` line for line. The script covers:
- initialize and tools/list;
- `board.load` of the divider;
- `dfm.check` of it against JLCPCB: two real FAILs. J1.2's plated pad is 0.26 mm from two tracks, and JLCPCB requires 0.28; confirmed independently.
- `excellon.check` of the real KiCad 9.0.6 drill file against the board: PASS. Of the broken test file: one INVALID_INPUT, three missing holes and one extra hole.
- `export.fab` through the fake CLI: its drill file omits J1.3, and exactly that hole is reported missing. A second export into the same folder gets `OUTPUT_EXISTS`.
- every error kind;
- a notification (no response);
- shutdown, a refusal after it, and exit.

The test also checks:
- each response is one line of JSON-RPC with exactly one of `result` and `error`;
- the session log holds all 25 requests and 23 responses.

**`tool_server_stdio` test:** the real `copper-core serve` process, fed `stdio_requests.jsonl` on stdin.
- Its stdout must equal `stdio_responses.jsonl`.
- The request after `exit` must never be read.
- The `--log` file must hold 15 lines.

**Sanitizers:** clean under UBSan in trap mode. ASan was not run; it is unavailable on MinGW.

**Regenerating the goldens:** `tool_server_test --write-golden`, and `cmake -DWRITE_GOLDEN=1 … -P tests/tool_server_stdio.cmake`. Review the diff before committing.

## Not yet

- **Batches:** JSON-RPC batch requests and positional (array) params are refused with a hint, not supported.
- **Responses are synchronous:** a long `export.fab` blocks the next request, bounded by the kicad-cli timeout.
- **Side effect of real exports:** with a real kicad-cli, `export.fab` lets it write the board's `.kicad_prl` next to the board (track D note).
