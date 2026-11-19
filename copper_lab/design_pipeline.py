"""Copper Lab structured-intent design pipeline.

Qwen emits copper.design-intent.v1 JSON. The deterministic compiler then
resolves library assets, writes native KiCad files, runs KiCad CLI, and imports
through the existing Copper Release executable.
"""
from __future__ import annotations

import datetime as dt
import importlib.util
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path
from typing import Any

APP_DIR = Path(__file__).resolve().parent
ROOT_DIR = APP_DIR.parent
V2_DIR = (ROOT_DIR / "src" / "v2_src" if (ROOT_DIR / "src" / "v2_src").is_dir() else ROOT_DIR / "v2_src")   # repository layout first, bundle layout second
TOOLS_DIR = V2_DIR / "tools"
COMPILER = TOOLS_DIR / "copper_design_compile.py"
ASSET_CONTEXT_TOOL = TOOLS_DIR / "kicad_asset_context.py"
SCHEMA_PATH = V2_DIR / "schemas" / "copper.design-intent.v1.schema.json"
SYSTEM_PROMPT_PATH = ROOT_DIR / "prompts" / "COPPER_DESIGN_INTENT_SYSTEM.md"
OLLAMA_URL = os.environ.get("OLLAMA_HOST", "http://127.0.0.1:11434").rstrip("/")

# Standard KiCad IDs used only as candidate context when a free-form request
# does not name exact library IDs. Runtime asset inspection filters missing IDs.
DEFAULT_CANDIDATES = [
    ("Connector_Generic:Conn_01x02", "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical"),
    ("Connector_Generic:Conn_01x03", "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical"),
    ("Regulator_Linear:AMS1117-3.3", "Package_TO_SOT_SMD:SOT-223-3_TabPin2"),
    ("Device:R", "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm"),
    ("Device:LED", "LED_THT:LED_D5.0mm"),
    ("Device:R", "Resistor_SMD:R_0603_1608Metric"),
    ("Device:LED", "LED_SMD:LED_0603_1608Metric"),
    ("Device:C", "Capacitor_SMD:C_0805_2012Metric"),
]


def utc_stamp() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def read_text(path: Path, fallback: str = "") -> str:
    return path.read_text(encoding="utf-8-sig") if path.is_file() else fallback


def read_copper_request_draft(path: Path) -> tuple[dict[str, Any], str]:
    """Read and normalize the JSON emitted by the Windows Copper CLI.

    The C CLI currently writes argv text using the active Windows narrow
    character encoding. When a prompt contains characters such as U+00D7
    (multiplication sign), the JSON file may therefore be CP1252 rather than
    UTF-8. Decode UTF-8 first, fall back to CP1252, parse as JSON, then rewrite
    the file as canonical UTF-8 so every downstream stage has one encoding.
    This fallback is intentionally limited to the Copper-generated request
    file; model responses and schema files remain strict UTF-8.
    """
    raw = path.read_bytes()
    source_encoding = "utf-8"
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        try:
            text = raw.decode("cp1252")
            source_encoding = "windows-1252"
        except UnicodeDecodeError as exc:
            raise RuntimeError(
                f"Copper request JSON is neither UTF-8 nor Windows-1252: {path} "
                f"(invalid byte at offset {exc.start})"
            ) from exc
    try:
        value = json.loads(text)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Copper request JSON is malformed after {source_encoding} decoding: {exc}") from exc
    if not isinstance(value, dict) or value.get("schema") != "copper.request-draft":
        raise RuntimeError("Copper CLI output is not a copper.request-draft JSON object")
    canonical = json.dumps(value, ensure_ascii=False, indent=2) + "\n"
    path.write_text(canonical, encoding="utf-8", newline="\n")
    return value, source_encoding


def library_roots() -> tuple[Path, Path]:
    program_files = Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
    base = program_files / "KiCad" / "9.0" / "share" / "kicad"
    symbols = Path(os.environ.get("COPPER_SYMBOLS_ROOT", str(base / "symbols")))
    footprints = Path(os.environ.get("COPPER_FOOTPRINTS_ROOT", str(base / "footprints")))
    if not symbols.is_dir():
        raise RuntimeError(f"KiCad symbol library directory not found: {symbols}; set COPPER_SYMBOLS_ROOT")
    if not footprints.is_dir():
        raise RuntimeError(f"KiCad footprint library directory not found: {footprints}; set COPPER_FOOTPRINTS_ROOT")
    return symbols.resolve(), footprints.resolve()


def collect_asset_context(prompt: str, symbols_root: Path, footprints_root: Path) -> dict[str, Any]:
    """Read real symbol pins and footprint pads; never ask the model to invent them."""
    if not ASSET_CONTEXT_TOOL.is_file():
        raise RuntimeError(f"KiCad asset context tool missing: {ASSET_CONTEXT_TOOL}")
    spec = importlib.util.spec_from_file_location("kicad_asset_context", ASSET_CONTEXT_TOOL)
    if not spec or not spec.loader:
        raise RuntimeError("Could not load KiCad asset context helper")
    sys.path.insert(0, str(TOOLS_DIR))
    try:
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        symbol_ids = sorted(set(module.SYMBOL_RE.findall(prompt)))
        footprint_ids = sorted(set(module.FOOTPRINT_RE.findall(prompt)))
        if symbol_ids or footprint_ids:
            return module.build_context(prompt, symbols_root, footprints_root)

        combined: dict[str, Any] = {
            "schema": "copper.kicad-library-context", "schema_version": 1,
            "source": "locally installed KiCad 9 library files",
            "symbols": [], "footprints": [], "notices": [], "candidate_pairs": [],
        }
        seen_symbols: set[str] = set()
        seen_footprints: set[str] = set()
        errors: list[str] = []
        for symbol_id, footprint_id in DEFAULT_CANDIDATES:
            try:
                one = module.build_context(
                    f"symbol {symbol_id} footprint {footprint_id}", symbols_root, footprints_root
                )
                for item in one["symbols"]:
                    if item["id"] not in seen_symbols:
                        combined["symbols"].append(item); seen_symbols.add(item["id"])
                for item in one["footprints"]:
                    if item["id"] not in seen_footprints:
                        combined["footprints"].append(item); seen_footprints.add(item["id"])
                combined["candidate_pairs"].append({"symbol_id": symbol_id, "footprint_id": footprint_id})
            except Exception as exc:
                errors.append(f"{symbol_id} / {footprint_id}: {exc}")
        if not combined["symbols"] or not combined["footprints"]:
            raise RuntimeError("No default KiCad library candidates could be inspected. " + "; ".join(errors))
        combined["notices"].append(
            "No library IDs were specified in the task. The model must choose only from candidate_pairs, "
            "and must use the exact pin/pad metadata supplied."
        )
        if errors:
            combined["unavailable_candidates"] = errors
        return combined
    finally:
        try:
            sys.path.remove(str(TOOLS_DIR))
        except ValueError:
            pass


def reject_explicitly_unavailable_assets(prompt: str, asset_context: dict[str, Any]) -> None:
    """Do not let the model silently omit or substitute a specifically requested unsupported library part."""
    prompt_folded = prompt.casefold()
    for message in asset_context.get("unavailable_candidates", []):
        # Candidate diagnostics start with `SymbolID / FootprintID:`.
        match = re.match(r"\s*([A-Za-z0-9_-]+:[A-Za-z0-9_.+-]+)\s*/\s*([A-Za-z0-9_-]+:[A-Za-z0-9_.+-]+):", str(message))
        if not match:
            continue
        symbol_id, footprint_id = match.groups()
        symbol_entry = symbol_id.split(":", 1)[1]
        footprint_entry = footprint_id.split(":", 1)[1]
        def explicitly_named(entry: str) -> bool:
            if len(entry) < 4:
                return False
            return re.search(r"(?<![A-Za-z0-9])" + re.escape(entry) + r"(?![A-Za-z0-9])", prompt, re.I) is not None
        if explicitly_named(symbol_entry) or explicitly_named(footprint_entry):
            raise RuntimeError(
                f"The request explicitly names {symbol_id} / {footprint_id}, but this local asset "
                f"cannot be used by Copper compiler v1: {message}. No substitute was selected."
            )


def post_ollama(payload: dict[str, Any], timeout: int = 900) -> dict[str, Any]:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        f"{OLLAMA_URL}/api/chat", data=body,
        headers={"Content-Type": "application/json"}, method="POST"
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read(32 * 1024 * 1024)
    except urllib.error.HTTPError as exc:
        body_text = exc.read().decode("utf-8", "replace")
        raise RuntimeError(f"Ollama HTTP {exc.code}: {body_text}") from exc
    except Exception as exc:
        raise RuntimeError(f"Ollama request failed: {exc}") from exc
    try:
        result = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"Ollama returned invalid JSON envelope: {exc}") from exc
    if not isinstance(result, dict):
        raise RuntimeError("Ollama response envelope is not an object")
    return result


def run_process(command: list[str], cwd: Path | None = None, timeout: int = 900) -> dict[str, Any]:
    started = time.time()
    try:
        proc = subprocess.run(command, cwd=str(cwd) if cwd else None,
                              capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=timeout)
        return {"returncode": proc.returncode, "stdout": proc.stdout,
                "stderr": proc.stderr, "elapsed_seconds": round(time.time() - started, 3),
                "command": command}
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout.decode("utf-8", "replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        stderr = exc.stderr.decode("utf-8", "replace") if isinstance(exc.stderr, bytes) else (exc.stderr or "")
        return {"returncode": None, "stdout": stdout, "stderr": stderr,
                "elapsed_seconds": round(time.time() - started, 3), "command": command,
                "timeout": True}


def execute_design_generation(prompt: str, model: str, run_dir: Path,
                               copper: str | None, kicad: str | None) -> dict[str, Any]:
    if not prompt.strip():
        raise ValueError("Enter an engineering request first")
    if len(prompt) > 12000:
        raise ValueError("Prompt exceeds 12,000 characters")
    for path in (COMPILER, SCHEMA_PATH, SYSTEM_PROMPT_PATH):
        if not path.is_file():
            raise RuntimeError(f"Required structured compiler file missing: {path}")
    if not copper:
        raise RuntimeError("Copper executable not found. Expected build\\Release\\copper.exe or build\\copper.exe, or set COPPER_EXE")
    if not kicad:
        raise RuntimeError("KiCad CLI not found. Set KICAD_CLI or install KiCad 9")

    run_dir.mkdir(parents=True, exist_ok=True)
    started = time.time()
    checks: list[dict[str, Any]] = []
    request_dir = run_dir / "request"
    qwen_dir = run_dir / "qwen"
    projects_dir = run_dir / "projects"
    request_dir.mkdir(exist_ok=True); qwen_dir.mkdir(exist_ok=True); projects_dir.mkdir(exist_ok=True)
    (run_dir / "task_prompt.txt").write_text(prompt, encoding="utf-8")

    request_path = request_dir / "design_request.draft.json"
    request_cmd = [copper, "user", "design", "prompt", prompt, "--out", str(request_path)]
    request_result = run_process(request_cmd, cwd=run_dir, timeout=30)
    (run_dir / "copper_prompt_capture.log").write_text(request_result["stdout"] + request_result["stderr"], encoding="utf-8")
    checks.append({"name": "copper_prompt_capture", "status": "passed" if request_result["returncode"] == 0 else "failed", "output": request_result["stdout"] + request_result["stderr"]})
    if request_result["returncode"] != 0:
        raise RuntimeError("Copper request capture failed: " + request_result["stderr"])

    symbols_root, footprints_root = library_roots()
    asset_context = collect_asset_context(prompt, symbols_root, footprints_root)
    write_json(qwen_dir / "local_kicad_library_context.json", asset_context)
    reject_explicitly_unavailable_assets(prompt, asset_context)
    schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    system_prompt = SYSTEM_PROMPT_PATH.read_text(encoding="utf-8")
    request_object, request_source_encoding = read_copper_request_draft(request_path)
    request_json = json.dumps(request_object, ensure_ascii=False, indent=2)
    write_json(qwen_dir / "copper_request_encoding.json", {
        "source_encoding": request_source_encoding,
        "normalized_encoding": "utf-8",
        "normalization": "Copper CLI request draft decoded and rewritten as canonical UTF-8",
    })
    user_message = (
        "Engineering request:\n" + prompt +
        "\n\nCopper request draft JSON:\n" + request_json +
        "\n\nAuthoritative local KiCad library context (use only these IDs and exact pin/pad metadata):\n" +
        json.dumps(asset_context, ensure_ascii=False, separators=(",", ":")) +
        "\n\nReturn exactly one JSON object conforming to the supplied JSON Schema. "
        "Do not return KiCad S-expressions, source code, Markdown, or unlisted library IDs."
    )
    context_tokens = int(os.environ.get("COPPER_OLLAMA_CONTEXT", "4096"))
    payload = {
        "model": model, "stream": False, "keep_alive": "5m", "format": schema,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_message},
        ],
        "options": {"temperature": 0.1, "num_ctx": context_tokens, "num_predict": 2048, "top_p": 0.9},
    }
    write_json(qwen_dir / "ollama_request.json", payload)
    llm_started = time.time()
    try:
        response = post_ollama(payload, timeout=900)
    except Exception as exc:
        (qwen_dir / "ollama_error.txt").write_text(str(exc) + "\n", encoding="utf-8")
        raise
    write_json(qwen_dir / "ollama_response.json", response)
    message = response.get("message") if isinstance(response.get("message"), dict) else {}
    content = message.get("content", "")
    if not isinstance(content, str) or not content.strip():
        raise RuntimeError("Ollama returned an empty message.content")
    (qwen_dir / "qwen_response.raw.txt").write_text(content, encoding="utf-8")
    content = re.sub(r"<think>.*?</think>", "", content, flags=re.S).strip()
    if content.startswith("```"):
        content = re.sub(r"^```(?:json)?\s*", "", content, flags=re.I)
        content = re.sub(r"\s*```$", "", content)
    try:
        intent = json.loads(content)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Qwen response is not JSON: {exc}") from exc
    if not isinstance(intent, dict):
        raise RuntimeError("Qwen design intent must be a JSON object")
    intent_path = qwen_dir / "qwen_design_intent.json"
    write_json(intent_path, intent)
    llm_meta = {
        "model": model, "context": context_tokens,
        "prompt_eval_count": response.get("prompt_eval_count"),
        "eval_count": response.get("eval_count"),
        "total_duration_ns": response.get("total_duration"),
        "load_duration_ns": response.get("load_duration"),
        "elapsed_seconds": round(time.time() - llm_started, 3),
    }
    write_json(qwen_dir / "generation_metrics.json", llm_meta)
    checks.append({"name": "ollama_structured_design_intent", "status": "passed", "output": f"{model} returned copper.design-intent JSON in {llm_meta['elapsed_seconds']}s; output tokens={llm_meta.get('eval_count')}"})

    compiler_command = [
        sys.executable, str(COMPILER), "--intent", str(intent_path), "--out", str(projects_dir),
        "--symbols-root", str(symbols_root), "--footprints-root", str(footprints_root),
        "--copper-cli", copper, "--kicad-cli", kicad,
    ]
    compiler_result = run_process(compiler_command, cwd=ROOT_DIR, timeout=900)
    (run_dir / "compiler.stdout.log").write_text(compiler_result["stdout"], encoding="utf-8")
    (run_dir / "compiler.stderr.log").write_text(compiler_result["stderr"], encoding="utf-8")
    checks.append({"name": "copper_design_compiler", "status": "passed" if compiler_result["returncode"] == 0 else "failed",
                   "output": (compiler_result["stdout"] + "\n" + compiler_result["stderr"])[-12000:]})
    if compiler_result["returncode"] != 0:
        raise RuntimeError(f"Copper Design Compiler failed (exit {compiler_result['returncode']}). See compiler logs.")

    project_name = str(intent.get("project", {}).get("name", "design"))
    project_dir = projects_dir / project_name
    if not project_dir.is_dir():
        children = [p for p in projects_dir.iterdir() if p.is_dir()]
        if len(children) != 1:
            raise RuntimeError("Compiler succeeded but generated project directory could not be identified")
        project_dir = children[0]
    # Generate native SVG previews after compiler validation. ERC/DRC findings
    # are preserved as findings; only parser/export failures are hard failures.
    validation_dir = project_dir / "validation"
    validation_dir.mkdir(exist_ok=True)
    sch_files = list(project_dir.glob("*.kicad_sch"))
    pcb_files = list(project_dir.glob("*.kicad_pcb"))
    if sch_files and pcb_files:
        sch_svg_dir = validation_dir / "schematic_svg"
        sch_svg_dir.mkdir(exist_ok=True)
        preview_commands = [
            [kicad, "sch", "export", "svg", "--output", str(sch_svg_dir), "--exclude-drawing-sheet", str(sch_files[0])],
            [kicad, "pcb", "export", "svg", "--mode-single", "--output", str(validation_dir),
             "--layers", "F.Cu,F.SilkS,Edge.Cuts", "--fit-page-to-board", "--exclude-drawing-sheet", str(pcb_files[0])],
        ]
        for index, command in enumerate(preview_commands, start=1):
            outcome = run_process(command, cwd=project_dir, timeout=180)
            checks.append({"name": "kicad_schematic_svg" if index == 1 else "kicad_board_svg",
                           "status": "passed" if outcome["returncode"] == 0 else "failed",
                           "output": (outcome["stdout"] + "\n" + outcome["stderr"])[-6000:]})
        for src in sch_svg_dir.glob("*.svg"):
            (run_dir / "schematic_preview.svg").write_bytes(src.read_bytes())
            break
        for src in validation_dir.glob("*.svg"):
            if src.parent == validation_dir:
                (run_dir / "board_preview.svg").write_bytes(src.read_bytes())
                break

    # Promote high-value artifacts to the run root for the preview and easy download.
    for src in [*project_dir.glob("*.kicad_sch"), *project_dir.glob("*.kicad_pcb"), *project_dir.glob("*.kicad_pro")]:
        target = run_dir / src.name
        target.write_bytes(src.read_bytes())
    for rel in ["validation", ".copper"]:
        source_dir = project_dir / rel
        if source_dir.is_dir():
            for src in source_dir.rglob("*"):
                if src.is_file():
                    dest = run_dir / rel / src.relative_to(source_dir)
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(src.read_bytes())
    # Report each native KiCad CLI operation separately. ERC/DRC findings are
    # evidence, not a hard generation failure; netlist export must succeed.
    cli_results_path = validation_dir / "cli_results.json"
    if cli_results_path.is_file():
        try:
            cli_results = json.loads(cli_results_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            cli_results = []
        for item in cli_results if isinstance(cli_results, list) else []:
            command = item.get("command", [])
            label = "kicad_" + ("_".join(str(x) for x in command[:3]) or "command").replace(" ", "_")
            rc = item.get("returncode")
            is_netlist = "netlist" in command
            status = "passed" if rc == 0 else ("failed" if is_netlist else "findings")
            checks.append({"name": label, "status": status,
                           "output": ((item.get("stdout") or "") + "\n" + (item.get("stderr") or ""))[-6000:]})
    copper_log = project_dir / "copper_import.stdout.log"
    copper_error_log = project_dir / "copper_import.stderr.log"
    copper_output = read_text(copper_log) + read_text(copper_error_log)
    checks.append({"name": "copper_native_import", "status": "passed" if copper_log.is_file() else "unknown",
                   "output": copper_output[-6000:] or "Copper import log not found"})

    # Flatten manifest files recursively; API file access is constrained to this run root.
    files = []
    for path in sorted(run_dir.rglob("*")):
        if path.is_file() and path.name != "run_manifest.json":
            rel = path.relative_to(run_dir).as_posix()
            files.append({"name": rel, "size": path.stat().st_size, "kind": path.suffix.lower().lstrip(".") or "file"})
    failures = [c for c in checks if c.get("status") in ("failed", "error", "timeout")]
    findings = [c for c in checks if c.get("status") == "findings"]
    metadata = {
        "run_id": run_dir.name, "created_at": utc_stamp(), "status": "completed_with_findings" if (failures or findings) else "completed",
        "model": model, "prompt": prompt, "llm": llm_meta, "checks": checks,
        "files": files, "project_name": project_name,
        "project_directory": f"projects/{project_name}",
        "elapsed_seconds": round(time.time() - started, 3),
        "manufacturing_gate": "NOT APPROVED — generated draft; inspect ERC/DRC/DFM and review engineering assumptions",
        "pipeline": "qwen_json_intent -> copper_design_compile.py -> kicad_cli -> copper_cli_import",
    }
    write_json(run_dir / "run_manifest.json", metadata)
    return metadata
