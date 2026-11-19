import http.client
import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

APP = Path(__file__).resolve().parents[1]
ROOT = APP.parent
V2 = ROOT / "src" / "v2_src" if (ROOT / "src" / "v2_src").is_dir() else ROOT / "v2_src"
sys.path.insert(0, str(APP))
import server  # noqa: E402
import design_pipeline  # noqa: E402

FIXTURE_INTENT = json.loads((V2 / "tests" / "fixture_design_intent.json").read_text(encoding="utf-8"))


class WebPipelineIntegrationTests(unittest.TestCase):
    def test_copper_request_json_cp1252_is_normalized_to_utf8(self):
        with tempfile.TemporaryDirectory() as td:
            request = Path(td) / "design_request.draft.json"
            request.write_bytes(
                b'{"schema":"copper.request-draft","schema_version":1,'
                b'"status":"needs_structuring","raw_prompt":"35 mm \xd7 25 mm"}'
            )
            value, encoding = design_pipeline.read_copper_request_draft(request)
            self.assertEqual(encoding, "windows-1252")
            self.assertEqual(value["raw_prompt"], "35 mm \u00d7 25 mm")
            normalized = request.read_bytes()
            normalized_text = normalized.decode("utf-8")
            self.assertIn("35 mm \u00d7 25 mm", normalized_text)

    def test_web_generation_delegates_to_structured_compiler(self):
        with tempfile.TemporaryDirectory() as td:
            old_runs = server.RUNS_DIR
            server.RUNS_DIR = Path(td)
            try:
                expected = {"status": "completed", "checks": [], "files": []}
                with patch.object(server, "copper_exe", return_value="C:/Copper/copper.exe"), \
                     patch.object(server, "kicad_cli", return_value="C:/KiCad/kicad-cli.exe"), \
                     patch.object(server, "execute_design_generation", return_value=expected) as execute:
                    result = server.execute_generation("Create a 5V LED indicator", FIXTURE_INTENT)
                self.assertEqual(result, expected)
                args = execute.call_args.args
                self.assertEqual(args[0], "Create a 5V LED indicator")
                self.assertEqual(args[1], FIXTURE_INTENT)
                self.assertEqual(args[3], "C:/Copper/copper.exe")
                self.assertEqual(args[4], "C:/KiCad/kicad-cli.exe")
                self.assertTrue(args[2].is_dir())
            finally:
                server.RUNS_DIR = old_runs

    def test_web_generation_persists_compiler_failure(self):
        with tempfile.TemporaryDirectory() as td:
            old_runs = server.RUNS_DIR
            server.RUNS_DIR = Path(td)
            try:
                with patch.object(server, "copper_exe", return_value="copper.exe"), \
                     patch.object(server, "kicad_cli", return_value="kicad-cli.exe"), \
                     patch.object(server, "execute_design_generation", side_effect=RuntimeError("test compiler failure")):
                    result = server.execute_generation("test", FIXTURE_INTENT)
                self.assertEqual(result["status"], "generation_failed")
                self.assertIn("test compiler failure", result["error"])
                manifest = json.loads((Path(td) / result["run_id"] / "run_manifest.json").read_text())
                self.assertEqual(manifest["status"], "generation_failed")
                self.assertEqual(manifest["author"], "manus")
            finally:
                server.RUNS_DIR = old_runs

    def test_local_asset_fallback_uses_only_assets_that_exist(self):
        fixture = V2 / "tests" / "fixtures" / "catalogue_resolution" / "kicad"
        context = design_pipeline.collect_asset_context(
            "Create a simple resistor-based circuit", fixture / "symbols", fixture / "footprints"
        )
        self.assertEqual(context["schema"], "copper.kicad-library-context")
        self.assertIn("Device:R", [item["id"] for item in context["symbols"]])
        self.assertIn("Resistor_SMD:R_0603_1608Metric", [item["id"] for item in context["footprints"]])
        self.assertTrue(context["candidate_pairs"])


    def test_structured_pipeline_orchestration_with_supplied_intent_and_mocked_kicad(self):
        fixture_json = V2 / "tests" / "fixture_design_intent.json"
        intent = json.loads(fixture_json.read_text(encoding="utf-8"))
        symbols = V2 / "tests" / "fixtures" / "catalogue_resolution" / "kicad" / "symbols"
        footprints = V2 / "tests" / "fixtures" / "catalogue_resolution" / "kicad" / "footprints"

        def fake_process(command, cwd=None, timeout=900):
            if "check-intent" in command:
                return {"returncode": 0, "stdout": "Design intent: GROUNDED (2 component(s), 4 pin(s), 2 net(s))", "stderr": "", "elapsed_seconds": 0.01}
            if "user" in command and "prompt" in command:
                out_path = Path(command[command.index("--out") + 1])
                out_path.parent.mkdir(parents=True, exist_ok=True)
                out_path.write_text('{"schema":"copper.request-draft","raw_prompt":"test"}', encoding="utf-8")
                return {"returncode": 0, "stdout": "prompt captured", "stderr": "", "elapsed_seconds": 0.01}
            if "copper_design_compile.py" in command[1]:
                out = Path(command[command.index("--out") + 1])
                project = out / intent["project"]["name"]
                validation = project / "validation"
                copper_dir = project / ".copper"
                validation.mkdir(parents=True, exist_ok=True)
                copper_dir.mkdir(parents=True, exist_ok=True)
                for suffix in (".kicad_sch", ".kicad_pcb", ".kicad_pro"):
                    (project / (intent["project"]["name"] + suffix)).write_text("(test)", encoding="utf-8")
                (validation / "cli_results.json").write_text(json.dumps([
                    {"command": ["sch", "export", "netlist"], "returncode": 0, "stdout": "netlist ok", "stderr": ""},
                    {"command": ["sch", "erc"], "returncode": 0, "stdout": "erc ok", "stderr": ""},
                    {"command": ["pcb", "drc"], "returncode": 0, "stdout": "drc ok", "stderr": ""},
                ]), encoding="utf-8")
                (validation / "erc.json").write_text(json.dumps({"kicad_version": "9.0.6", "sheets": [{"violations": []}]}), encoding="utf-8")
                (validation / "drc.json").write_text(json.dumps({"kicad_version": "9.0.6", "violations": [], "unconnected_items": [
                    {"type": "unconnected_items", "severity": "error"}], "schematic_parity": []}), encoding="utf-8")
                (copper_dir / "design.json").write_text('{"schema":"copper.engineering-design.v1"}', encoding="utf-8")
                (project / "copper_import.stdout.log").write_text("Copper design import: OK", encoding="utf-8")
                (project / "copper_import.stderr.log").write_text("", encoding="utf-8")
                return {"returncode": 0, "stdout": "KiCad project generated", "stderr": "", "elapsed_seconds": 0.02}
            if "sch" in command and "svg" in command:
                out = Path(command[command.index("--output") + 1]); out.mkdir(parents=True, exist_ok=True)
                (out / "schematic.svg").write_text("<svg/>", encoding="utf-8")
            if "pcb" in command and "svg" in command:
                out = Path(command[command.index("--output") + 1])   # --mode-single: a file name
                self.assertEqual(out.suffix, ".svg")
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_text("<svg/>", encoding="utf-8")
            return {"returncode": 0, "stdout": "preview ok", "stderr": "", "elapsed_seconds": 0.01}

        with tempfile.TemporaryDirectory() as td:
            run_dir = Path(td) / "run"; run_dir.mkdir()
            with patch.object(design_pipeline, "library_roots", return_value=(symbols, footprints)), \
                 patch.object(design_pipeline, "run_process", side_effect=fake_process):
                manifest = design_pipeline.execute_design_generation("Build the fixture circuit", intent, run_dir, "copper.exe", "kicad-cli.exe")
            # ERC is clean in the fixture report; DRC lists one unconnected item, so it is a finding, not a pass,
            # even though kicad-cli exited 0.
            by_name = {c["name"]: c for c in manifest["checks"]}
            self.assertEqual(by_name["kicad_erc"]["status"], "passed")
            self.assertEqual(by_name["kicad_drc"]["status"], "findings")
            self.assertIn("unconnected_items x1", by_name["kicad_drc"]["output"])
            self.assertEqual(manifest["status"], "completed_with_findings")
            self.assertEqual(manifest["author"], "manus")
            self.assertEqual(json.loads((run_dir / "intent" / "design_intent.json").read_text(encoding="utf-8")), intent)
            self.assertTrue((run_dir / "CopperCompilerFixture.kicad_sch").is_file())
            self.assertTrue((run_dir / "CopperCompilerFixture.kicad_pcb").is_file())
            self.assertTrue((run_dir / "schematic_preview.svg").is_file())
            self.assertTrue((run_dir / "board_preview.svg").is_file())
            self.assertTrue(any(f["name"] == "validation/cli_results.json" for f in manifest["files"]))

    def test_grounding_gate_rejection_stops_before_the_compiler(self):
        symbols = V2 / "tests" / "fixtures" / "catalogue_resolution" / "kicad" / "symbols"
        footprints = V2 / "tests" / "fixtures" / "catalogue_resolution" / "kicad" / "footprints"
        calls = []

        def fake_process(command, cwd=None, timeout=900):
            calls.append(command)
            if "check-intent" in command:
                return {"returncode": 2, "stdout": "Design intent: REJECTED\n  [PIN_NAME_MISMATCH] D1 1: ...", "stderr": "", "elapsed_seconds": 0.01}
            return {"returncode": 0, "stdout": "", "stderr": "", "elapsed_seconds": 0.01}

        with tempfile.TemporaryDirectory() as td:
            with patch.object(design_pipeline, "library_roots", return_value=(symbols, footprints)), \
                 patch.object(design_pipeline, "run_process", side_effect=fake_process):
                with self.assertRaisesRegex(RuntimeError, "Grounding gate rejected"):
                    design_pipeline.execute_design_generation("", FIXTURE_INTENT, Path(td) / "run", "copper.exe", "kicad-cli.exe")
        self.assertFalse(any("copper_design_compile.py" in " ".join(map(str, c)) for c in calls), "the compiler must not run")

    def test_explicit_unavailable_library_part_is_not_silently_substituted(self):
        context = {
            "unavailable_candidates": [
                "Regulator_Linear:AMS1117-3.3 / Package_TO_SOT_SMD:SOT-223-3_TabPin2: derived symbol not supported in compiler v1"
            ]
        }
        with self.assertRaisesRegex(RuntimeError, "No substitute was selected"):
            design_pipeline.reject_explicitly_unavailable_assets(
                "Build a 5V to 3.3V regulator board using AMS1117-3.3", context
            )

    def test_no_language_model_is_called(self):
        # Manus is the only design author: Copper Lab has no Qwen/Ollama path left.
        for name in ("server.py", "design_pipeline.py", "static/app.js", "static/index.html"):
            source = (APP / name).read_text(encoding="utf-8").lower()
            self.assertNotIn("ollama", source, name)
            self.assertNotIn("qwen", source, name)
        source = (APP / "server.py").read_text(encoding="utf-8")
        self.assertIn("execute_design_generation(prompt, intent, run_dir, copper_exe(), kicad_cli())", source)

    def test_supplied_intent_shape_is_checked_before_anything_runs(self):
        with tempfile.TemporaryDirectory() as td:
            for bad, message in (([], "JSON object"), ({"schema": "other", "schema_version": 1, "components": [1]}, "copper.design-intent"),
                                 ({"schema": "copper.design-intent", "schema_version": 1, "components": []}, "no components")):
                with self.assertRaisesRegex(ValueError, message):
                    design_pipeline.execute_design_generation("x", bad, Path(td) / "run", "copper.exe", "kicad-cli.exe")
                self.assertFalse((Path(td) / "run").exists())

    def test_http_api_is_manus_only_and_json_only(self):
        httpd = server.ThreadingHTTPServer(("127.0.0.1", 0), server.Handler)
        thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        thread.start()
        port = httpd.server_address[1]

        def call(method, path, body=None, ctype="application/json"):
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
            headers = {"Content-Type": ctype} if ctype else {}
            conn.request(method, path, body=body, headers=headers)
            resp = conn.getresponse()
            data = resp.read()
            conn.close()
            return resp.status, (json.loads(data) if data else None)

        try:
            status, body = call("GET", "/api/status")
            self.assertEqual(status, 200)
            self.assertNotIn("ollama", body)
            self.assertEqual(body["design_author"], "manus")
            payload = json.dumps({"intent": FIXTURE_INTENT}).encode()
            for ctype in ("text/plain", "application/x-www-form-urlencoded", None):
                status, body = call("POST", "/api/compile", payload, ctype)
                self.assertEqual(status, 415, ctype)
            status, body = call("POST", "/api/compile", json.dumps({"intent": "{not json"}).encode())
            self.assertEqual(status, 400)
            self.assertIn("not valid JSON", body["error"])
            status, body = call("POST", "/api/compile", json.dumps({"prompt": "no intent"}).encode())
            self.assertEqual(status, 400)
            status, body = call("POST", "/api/nowhere", payload)
            self.assertEqual(status, 404)
        finally:
            httpd.shutdown()
            httpd.server_close()


if __name__ == "__main__":
    unittest.main()
