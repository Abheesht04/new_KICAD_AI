# kicad-cli runner (V2 track D)

`src/v2_src/kicad_cli.{c,h}` runs KiCad's command-line tool from C. It is in the `copper_v2` library. From the command line, `copper-core kicad drc|gerbers|drill|pos BOARD OUT` prints the result as JSON.

## Jobs

| Function | kicad-cli arguments | Accepted exit codes |
|---|---|---|
| `kicad_cli_open` | `version` | 0; the version is parsed (e.g. 9.0.6) |
| `kicad_cli_drc` | `pcb drc --format json --exit-code-violations [--schematic-parity] [--all-track-errors] -o FILE BOARD` | 0, and 5 (violations; the report is still written, and `drc_violations` is set) |
| `kicad_cli_gerbers` | `pcb export gerbers -o DIR/ BOARD` | 0 |
| `kicad_cli_drill` | `pcb export drill --format excellon --drill-origin absolute --excellon-zeros-format decimal --excellon-units mm -o DIR/ BOARD` (the format `excellon.c` reads) | 0 |
| `kicad_cli_pos` | `pcb export pos --format csv --units mm --side both -o FILE BOARD` | 0 |

**Finding kicad-cli:** an explicit path, then `KICAD_CLI_PATH`, then `PATH`, then the KiCad 9 install folders (shared with `copper_project`).

## Guarantees

- **No shell:**
  - **Windows:** `CreateProcessW` gets argv directly, each argument quoted by the `CommandLineToArgvW` rules and converted from UTF-8 to UTF-16.
  - **POSIX:** `posix_spawn`.
  - **Tested:** arguments with spaces, quotes, trailing backslashes and UTF-8 arrive whole.
- **Timeout that kills the whole tree:**
  - the child runs in its own Windows job object or POSIX process group;
  - on expiry the whole tree is killed (`TerminateJobObject`, `kill(-pgid, SIGKILL)`);
  - the result is `KCLI_TIMEOUT`, with exit code −1.
- **No deadlock on large output:** stdout and stderr go to files in the staging folder, not to pipes. The test pushes 4 MiB through each.
- **Never overwrites:**
  - the output path must not exist, or the result is `KCLI_EXISTS` and nothing is launched;
  - kicad-cli writes into a fresh staging folder, `<output>.copper-staging-<pid>-<n>`, beside the output;
  - on success the output is moved into place with a rename that refuses to replace: `MoveFileExW` without `REPLACE_EXISTING`, `link` + `unlink` for files, and a checked rename for folders on POSIX;
  - on any failure the staging folder is removed and the output path stays absent;
  - an output that appears while kicad-cli runs is `KCLI_EXISTS`, not overwritten.
- **Result record:** the result keeps the exact argv, exit code, elapsed ms, captured stdout and stderr, the files produced (sorted), and the CLI's version.
- **Version gate:** KiCad 9.x is the only native evidence (CLAUDE.md).
  - With `require_9x`, any other version is `KCLI_WRONG_VERSION` before a job runs.
  - Without it, `cli_is_9x` and `cli_version` say which version ran.
  - A version that cannot be parsed is `KCLI_WRONG_VERSION`.
- **Failure statuses:** `KCLI_NOT_FOUND`, `KCLI_LAUNCH_FAILED`, `KCLI_TOOL_FAILED` (an exit code the job does not accept), `KCLI_NO_OUTPUT` (an accepted exit code but no output), `KCLI_INVALID_ARGUMENT` (for example a missing board), `KCLI_IO_ERROR`, `KCLI_OUT_OF_MEMORY`.

## Evidence

**`kicad_cli` test, against `tests/fake_kicad_cli.c`:** the fake records its argv, and each mode exercises one path:
- every job, into paths with spaces and a UTF-8 folder name;
- a second run into the same output is refused, and the first output is unchanged;
- exit 5 is accepted as DRC violations;
- exit 3 gives `TOOL_FAILED`, with stderr captured and no output;
- missing output gives `NO_OUTPUT`;
- a 60-second hang is killed after 300 ms;
- 4 MiB each on stdout and stderr is captured whole;
- the version gate rejects 10.0.5 and unparseable versions;
- no staging folder is ever left behind.

**`kicad_cli_real` test**, run with `KICAD_CLI` set; it skips as UNVERIFIED otherwise. All four jobs run on the Copper-generated `r0603x5` board:
- **DRC:** the report parses with `json.c`.
- **Gerbers:** the output includes a job file.
- **Drill:** the file, read back by `excellon.c`, holds exactly the board model's three J1 holes.
- **Positions:** the CSV lists R1–R5 and J1.
- **Version used:** on this machine the CLI is KiCad 10.0.5, so this is a development check, not KiCad 9 evidence. KiCad 10 reports DRC violations on that board (exit 5); the runner records them.

**Sanitizers:** both tests run clean under UBSan in trap mode. ASan was not run; it is unavailable on MinGW.

## Known limits

- **Windows executables:** the CLI must be a real `.exe`. A `.bat` or `.cmd` wrapper is not run, because that would need a shell.
- **Non-ASCII paths on Windows:**
  - `exc_parse_path` (the protected `excellon` module) and `filesystem.c` use the ANSI file APIs, so they cannot open non-ASCII paths on Windows;
  - the runner itself handles them; to read such a file, load it into memory and call `exc_parse`.
- **Files next to the input board:** kicad-cli itself may write the board's project-local settings (`NAME.kicad_prl`) beside the board it opens. That file is not part of the output, and the runner cannot prevent it. To keep a source tree untouched, run on a copy, as the real-CLI test does.
- **Staging-name counter:** the counter is a process-global, so jobs from several threads at once are not supported.
- **Python still calls kicad-cli:** `tools/copper.py native_gates` still runs `kicad-cli` from Python. Moving it onto this runner is port step 9 in [C_FIRST_AUDIT.md](C_FIRST_AUDIT.md).
