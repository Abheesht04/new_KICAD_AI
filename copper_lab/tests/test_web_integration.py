import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

APP = Path(__file__).resolve().parents[1]
ROOT = APP.parent
V2 = ROOT / "src" / "v2_src" if (ROOT / "src" / "v2_src").is_dir() else ROOT / "v2_src"
sys.path.insert(0, str(APP))
import server  # noqa: E402
import design_pipeline  # noqa: E402


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
                    result = server.execute_generation("Create a 5V LED indicator", "qwen3:8b")
                self.assertEqual(result, expected)
                args = execute.call_args.args
                self.assertEqual(args[0], "Create a 5V LED indicator")
                self.assertEqual(args[1], "qwen3:8b")
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
                    result = server.execute_generation("test", "llama3.2:latest")
                self.assertEqual(result["status"], "generation_failed")
                self.assertIn("test compiler failure", result["error"])
                manifest = json.loads((Path(td) / result["run_id"] / "run_manifest.json").read_text())
                self.assertEqual(manifest["status"], "generation_failed")
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


    def test_structured_pipeline_orchestration_with_mocked_ollama_and_kicad(self):
        fixture_json = V2 / "tests" / "fixture_design_intent.json"
        intent = json.loads(fixture_json.read_text(encoding="utf-8"))
        symbols = V2 / "tests" / "fixtures" / "catalogue_resolution" / "kicad" / "symbols"
        footprints = V2 / "tests" / "fixtures" / "catalogue_resolution" / "kicad" / "footprints"

        def fake_process(command, cwd=None, timeout=900):
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
                (copper_dir / "design.json").write_text('{"schema":"copper.engineering-design.v1"}', encoding="utf-8")
                (project / "copper_import.stdout.log").write_text("Copper design import: OK", encoding="utf-8")
                (project / "copper_import.stderr.log").write_text("", encoding="utf-8")
                return {"returncode": 0, "stdout": "KiCad project generated", "stderr": "", "elapsed_seconds": 0.02}
            if "sch" in command and "svg" in command:
                out = Path(command[command.index("--output") + 1]); out.mkdir(parents=True, exist_ok=True)
                (out / "schematic.svg").write_text("<svg/>", encoding="utf-8")
            if "pcb" in command and "svg" in command:
                out = Path(command[command.index("--output") + 1]); out.mkdir(parents=True, exist_ok=True)
                (out / "board.svg").write_text("<svg/>", encoding="utf-8")
            return {"returncode": 0, "stdout": "preview ok", "stderr": "", "elapsed_seconds": 0.01}

        with tempfile.TemporaryDirectory() as td:
            run_dir = Path(td) / "run"; run_dir.mkdir()
            with patch.object(design_pipeline, "library_roots", return_value=(symbols, footprints)), \
                 patch.object(design_pipeline, "post_ollama", return_value={"message": {"content": json.dumps(intent)}, "eval_count": 100}), \
                 patch.object(design_pipeline, "run_process", side_effect=fake_process):
                manifest = design_pipeline.execute_design_generation("Build the fixture circuit", "llama3.2:latest", run_dir, "copper.exe", "kicad-cli.exe")
            self.assertEqual(manifest["status"], "completed")
            self.assertTrue((run_dir / "CopperCompilerFixture.kicad_sch").is_file())
            self.assertTrue((run_dir / "CopperCompilerFixture.kicad_pcb").is_file())
            self.assertTrue((run_dir / "schematic_preview.svg").is_file())
            self.assertTrue((run_dir / "board_preview.svg").is_file())
            self.assertTrue(any(f["name"] == "validation/cli_results.json" for f in manifest["files"]))

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

    def test_old_raw_sexpr_generation_path_is_not_used(self):
        source = (APP / "server.py").read_text(encoding="utf-8")
        self.assertNotIn("def call_qwen(", source)
        self.assertIn("execute_design_generation(prompt, model, run_dir, copper_exe(), kicad_cli())", source)


if __name__ == "__main__":
    unittest.main()
