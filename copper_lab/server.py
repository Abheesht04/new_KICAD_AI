#!/usr/bin/env python3
"""Copper Lab: small localhost wrapper around the Copper CLI, the design compiler and KiCad CLI.

Designs come from a Manus agent (or a person) as copper.design-intent JSON; Copper Lab runs no language model.
Only fixed, allow-listed commands are executed. This is intentionally a local tool, not a network-facing service:
it binds to loopback, and POSTs must be application/json so other web pages cannot drive it through a browser.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

from design_pipeline import collect_asset_context, execute_design_generation, library_roots, reject_explicitly_unavailable_assets

APP_DIR = Path(__file__).resolve().parent
ROOT_DIR = APP_DIR.parent
SOURCE_DIR = (ROOT_DIR / "src" / "v2_src" if (ROOT_DIR / "src" / "v2_src").is_dir() else ROOT_DIR / "v2_src")   # repository layout first, bundle layout second
RUNS_DIR = APP_DIR / "runs"
STATIC_DIR = APP_DIR / "static"
FIXTURE_DIR = SOURCE_DIR / "tests" / "fixtures" / "sample_resistor_lol"
MAX_PROMPT_CHARS = 12000
MAX_ARTIFACT_CHARS = 2_000_000
MAX_REQUEST_BYTES = 1_000_000


def utc_stamp() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


def find_executable(env_name: str, candidates: list[Path], command: str) -> str | None:
    override = os.environ.get(env_name)
    if override:
        p = Path(override).expanduser()
        if p.is_file():
            return str(p.resolve())
    # Known build locations before PATH: on Windows shutil.which also searches the current folder, where this
    # repository's copper.cmd launcher (not the Copper CLI) would otherwise win.
    for p in candidates:
        if p.is_file():
            return str(p.resolve())
    on_path = shutil.which(command)
    if on_path:
        return on_path
    return None


def copper_exe() -> str | None:
    return find_executable("COPPER_EXE", [
        ROOT_DIR / "build" / "Release" / "copper.exe",
        ROOT_DIR / "build" / "Release" / "copper",
        ROOT_DIR / "build" / "copper.exe",
        ROOT_DIR / "build" / "copper",
    ], "copper")


def kicad_cli() -> str | None:
    return find_executable("KICAD_CLI", [
        Path(r"C:\Program Files\KiCad\9.0\bin\kicad-cli.exe"),
        Path(r"C:\Program Files\KiCad\8.0\bin\kicad-cli.exe"),
        Path("/usr/bin/kicad-cli"),
        Path("/usr/local/bin/kicad-cli"),
    ], "kicad-cli")


def check_tool_version(exe: str | None, args: list[str]) -> dict[str, Any]:
    if not exe:
        return {"available": False, "version": None}
    try:
        proc = subprocess.run([exe, *args], capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=8)
        out = (proc.stdout or proc.stderr).strip()
        return {"available": proc.returncode == 0, "version": out[:240], "exit_code": proc.returncode}
    except Exception as exc:
        return {"available": False, "version": None, "error": str(exc)}


def get_status() -> dict[str, Any]:
    copper = copper_exe()
    kicad = kicad_cli()
    return {
        "copper": {"available": bool(copper), "path": copper,
                    **(check_tool_version(copper, ["--help"]) if copper else {})},
        "kicad": {"available": bool(kicad), "path": kicad,
                  **(check_tool_version(kicad, ["version"]) if kicad else {})},
        "design_author": "manus",
        "timestamp": utc_stamp(),
    }


def balanced_sexpr(text: str, expected_root: str) -> tuple[bool, str]:
    if not isinstance(text, str) or not text.strip():
        return False, "empty artifact"
    if len(text) > MAX_ARTIFACT_CHARS:
        return False, f"artifact exceeds {MAX_ARTIFACT_CHARS} character safety limit"
    stripped = text.lstrip()
    if not stripped.startswith("(" + expected_root):
        return False, f"expected top-level ({expected_root} ... )"
    depth = 0
    quoted = False
    escaped = False
    comment = False
    for i, ch in enumerate(text):
        if comment:
            if ch in "\r\n":
                comment = False
            continue
        if quoted:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                quoted = False
            continue
        if ch == '"':
            quoted = True
        elif ch == ';':
            comment = True
        elif ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth < 0:
                return False, f"unexpected closing parenthesis near character {i}"
    if quoted:
        return False, "unterminated quoted string"
    if depth != 0:
        return False, f"parenthesis balance is {depth}, expected 0"
    return True, "balanced S-expression (lexical check only)"


def write_json(path: Path, data: Any) -> None:
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def run_command(run_dir: Path, name: str, exe: str, args: list[str], timeout: int = 120) -> dict[str, Any]:
    started = time.time()
    try:
        proc = subprocess.run([exe, *args], cwd=str(run_dir), capture_output=True,
                              text=True, encoding="utf-8", errors="replace", timeout=timeout)
        stdout, stderr, code = proc.stdout or "", proc.stderr or "", proc.returncode
        combined = stdout + ("\n[stderr]\n" + stderr if stderr else "")
        result = {"name": name, "status": "passed" if code == 0 else "failed",
                  "exit_code": code, "elapsed_seconds": round(time.time() - started, 3),
                  "command": [Path(exe).name, *args], "output": combined[-30000:]}
    except subprocess.TimeoutExpired as exc:
        out = (exc.stdout or "")
        err = (exc.stderr or "")
        if isinstance(out, bytes): out = out.decode("utf-8", "replace")
        if isinstance(err, bytes): err = err.decode("utf-8", "replace")
        result = {"name": name, "status": "timeout", "exit_code": None,
                  "elapsed_seconds": round(time.time() - started, 3),
                  "command": [Path(exe).name, *args], "output": (out + "\n" + err)[-30000:]}
    except Exception as exc:
        result = {"name": name, "status": "error", "exit_code": None,
                  "elapsed_seconds": round(time.time() - started, 3),
                  "command": [Path(exe).name, *args], "output": str(exc)}
    log_name = re.sub(r"[^a-zA-Z0-9_-]", "_", name) + ".log"
    (run_dir / log_name).write_text(result.get("output", ""), encoding="utf-8", errors="replace")
    result["log_file"] = log_name
    return result


def run_manifest_files(run_dir: Path) -> list[dict[str, Any]]:
    files: list[dict[str, Any]] = []
    for p in sorted(run_dir.iterdir()):
        if not p.is_file() or p.name == "run_manifest.json":
            continue
        try:
            size = p.stat().st_size
        except OSError:
            continue
        files.append({"name": p.name, "size": size,
                      "kind": p.suffix.lower().lstrip(".") or "file"})
    return files


def execute_generation(prompt: str, intent: dict[str, Any]) -> dict[str, Any]:
    """Compile and check a design intent written by Manus (or a person). `prompt` is the request it answers."""
    if len(prompt) > MAX_PROMPT_CHARS:
        raise ValueError(f"Request text is limited to {MAX_PROMPT_CHARS} characters")
    if not isinstance(intent, dict):
        raise ValueError("Paste a copper.design-intent JSON object (written by Manus)")
    RUNS_DIR.mkdir(parents=True, exist_ok=True)
    run_id = dt.datetime.now().strftime("%Y%m%d_%H%M%S") + "_" + uuid.uuid4().hex[:6]
    run_dir = RUNS_DIR / run_id
    run_dir.mkdir(parents=False, exist_ok=False)
    try:
        return execute_design_generation(prompt, intent, run_dir, copper_exe(), kicad_cli())
    except Exception as exc:
        error_text = str(exc)
        (run_dir / "generation_error.txt").write_text(error_text + "\n", encoding="utf-8")
        files = []
        for path in sorted(run_dir.rglob("*")):
            if path.is_file() and path.name != "run_manifest.json":
                rel = path.relative_to(run_dir).as_posix()
                files.append({"name": rel, "size": path.stat().st_size,
                              "kind": path.suffix.lower().lstrip(".") or "file"})
        checks = []
        if (run_dir / "copper_prompt_capture.log").is_file():
            checks.append({"name": "copper_prompt_capture", "status": "passed",
                           "output": (run_dir / "copper_prompt_capture.log").read_text(encoding="utf-8", errors="replace")})
        manifest = {
            "run_id": run_id, "created_at": utc_stamp(), "status": "generation_failed",
            "author": "manus", "prompt": prompt, "error": error_text,
            "checks": checks, "files": files,
            "elapsed_seconds": None,
            "manufacturing_gate": "NOT APPROVED — generation or compilation failed",
            "pipeline": "design_intent (Manus) -> copper_design_compile.py -> kicad_cli -> copper_cli_import",
        }
        write_json(run_dir / "run_manifest.json", manifest)
        return manifest


class Handler(BaseHTTPRequestHandler):
    server_version = "CopperLab/0.1"

    def _send(self, status: int, body: bytes, content_type: str = "application/json; charset=utf-8") -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, status: int, payload: Any) -> None:
        self._send(status, json.dumps(payload, ensure_ascii=False).encode("utf-8"))

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path == "/api/status":
            self._json(200, get_status())
            return
        if parsed.path == "/api/runs":
            RUNS_DIR.mkdir(parents=True, exist_ok=True)
            runs = []
            for p in sorted(RUNS_DIR.iterdir(), reverse=True):
                manifest = p / "run_manifest.json"
                if p.is_dir() and manifest.is_file():
                    try:
                        item = json.loads(manifest.read_text(encoding="utf-8"))
                        runs.append({k: item.get(k) for k in ("run_id", "created_at", "status", "author", "elapsed_seconds")})
                    except Exception:
                        pass
            self._json(200, {"runs": runs[:30]})
            return
        if parsed.path == "/api/run":
            q = parse_qs(parsed.query)
            run_id = (q.get("id") or [""])[0]
            if not re.fullmatch(r"[0-9]{8}_[0-9]{6}_[a-f0-9]{6}", run_id):
                self._json(400, {"error": "Invalid run ID"}); return
            manifest = RUNS_DIR / run_id / "run_manifest.json"
            if not manifest.is_file():
                self._json(404, {"error": "Run not found"}); return
            self._send(200, manifest.read_bytes())
            return
        if parsed.path == "/api/file":
            q = parse_qs(parsed.query)
            run_id = (q.get("id") or [""])[0]
            name = (q.get("name") or [""])[0]
            normalized_name = name.replace("\\", "/")
            parts = normalized_name.split("/")
            if (not re.fullmatch(r"[0-9]{8}_[0-9]{6}_[a-f0-9]{6}", run_id)
                    or not normalized_name or normalized_name.startswith("/")
                    or any(part in ("", ".", "..") for part in parts)
                    or ":" in normalized_name):
                self._json(400, {"error": "Invalid artifact reference"}); return
            target = (RUNS_DIR / run_id / Path(*parts)).resolve()
            root = (RUNS_DIR / run_id).resolve()
            if root not in target.parents or not target.is_file():
                self._json(404, {"error": "Artifact not found"}); return
            data = target.read_bytes()
            if target.suffix.lower() in (".png", ".jpg", ".jpeg", ".svg"):
                ctype = {".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".svg": "image/svg+xml"}[target.suffix.lower()]
                self._send(200, data, ctype)
            else:
                self._send(200, data, "text/plain; charset=utf-8")
            return
        if parsed.path in ("/", "/index.html"):
            target = STATIC_DIR / "index.html"
        elif parsed.path.startswith("/static/"):
            relative = parsed.path.removeprefix("/static/")
            if Path(relative).name != relative:
                self._json(400, {"error": "Invalid static asset"}); return
            target = STATIC_DIR / relative
        else:
            self._json(404, {"error": "Not found"}); return
        if not target.is_file():
            self._json(404, {"error": "Static asset missing"}); return
        ctype = "text/html; charset=utf-8" if target.suffix == ".html" else ("text/javascript; charset=utf-8" if target.suffix == ".js" else "text/css; charset=utf-8")
        self._send(200, target.read_bytes(), ctype)

    def _read_json_body(self) -> dict[str, Any] | None:
        """The JSON body of a POST, or None after answering with an error. Content-Type must be application/json:
        a page on another site cannot send that type without a CORS preflight, which this server does not grant."""
        ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
        if ctype != "application/json":
            self._json(415, {"error": "POST needs Content-Type: application/json"})
            return None
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if length <= 0 or length > MAX_REQUEST_BYTES:
            self._json(413, {"error": "Request body missing or too large"})
            return None
        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
        except Exception as exc:
            self._json(400, {"error": f"Body is not JSON: {exc}"})
            return None
        if not isinstance(payload, dict):
            self._json(400, {"error": "Body must be a JSON object"})
            return None
        return payload

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path not in ("/api/compile", "/api/generate", "/api/context"):
            self._json(404, {"error": "Not found"})
            return
        payload = self._read_json_body()
        if payload is None:
            return
        if parsed.path == "/api/context":
            text = payload.get("text", "")
            if not isinstance(text, str) or not text.strip() or len(text) > MAX_PROMPT_CHARS:
                self._json(400, {"error": "text must name the parts, e.g. "
                                          "'symbol Device:R footprint Resistor_SMD:R_0603_1608Metric'"})
                return
            try:
                symbols, footprints = library_roots()
                context = collect_asset_context(text, symbols, footprints)
                reject_explicitly_unavailable_assets(text, context)
                self._json(200, context)
            except Exception as exc:
                self._json(422, {"error": str(exc)})
            return
        prompt = payload.get("prompt", "") or ""
        intent = payload.get("intent")
        if isinstance(intent, str):
            try:
                intent = json.loads(intent)
            except json.JSONDecodeError as exc:
                self._json(400, {"error": f"The design intent is not valid JSON: {exc}"})
                return
        if not isinstance(prompt, str) or not isinstance(intent, dict):
            self._json(400, {"error": 'Send {"intent": <copper.design-intent JSON from Manus>, '
                                      '"prompt": <optional request text>}'})
            return
        try:
            sys.stdout.write(f"[Copper Lab] POST {parsed.path} started | design intent from Manus\n")
            sys.stdout.flush()
            self._json(200, execute_generation(prompt, intent))
        except Exception as exc:
            self._json(500, {"error": str(exc)})

    def log_message(self, fmt: str, *args: Any) -> None:
        sys.stdout.write("[Copper Lab] " + (fmt % args) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description="Copper Lab local web wrapper")
    parser.add_argument("--host", default="127.0.0.1", help="Bind host (default: loopback only)")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    if args.host not in ("127.0.0.1", "localhost", "::1"):
        parser.error("Copper Lab intentionally binds only to loopback for this local prototype")
    RUNS_DIR.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Copper Lab listening at http://127.0.0.1:{args.port}")
    print(f"Python: {sys.executable}")
    print(f"Copper executable: {copper_exe() or 'NOT FOUND (set COPPER_EXE)'}")
    print(f"KiCad CLI: {kicad_cli() or 'NOT FOUND (set KICAD_CLI)'}")
    print(f"Design compiler: {SOURCE_DIR / 'tools' / 'copper_design_compile.py'}")
    print("Designs: copper.design-intent JSON written by Manus (paste it in the page or POST /api/compile)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping Copper Lab...")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
