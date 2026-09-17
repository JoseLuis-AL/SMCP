from __future__ import annotations

import importlib.machinery
import importlib.util
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import uuid
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "smcp"
loader = importlib.machinery.SourceFileLoader("smcp_cli", str(SCRIPT))
spec = importlib.util.spec_from_loader(loader.name, loader)
smcp = importlib.util.module_from_spec(spec)
loader.exec_module(smcp)


class SmcpCliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.models = self.root / "models"
        self.conda = self.root / "miniconda3/bin/conda"
        self.registry = self.models / "models.json"
        self.models.mkdir(parents=True)
        self.conda.parent.mkdir(parents=True)
        self.conda.touch()

        source_registry = json.loads((SCRIPT.parent / "models.json").read_text(encoding="utf-8"))
        self.registry.write_text(json.dumps(source_registry), encoding="utf-8")
        for model in source_registry["models"]:
            environment_python = self.conda.parent.parent / "envs" / model["environment"] / "bin/python"
            environment_python.parent.mkdir(parents=True)
            environment_python.touch()
            directory = self.models / model["directory"]
            for relative in [model["runner"], *model["required_files"]]:
                path = directory / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()

        self.environment = mock.patch.dict(os.environ, {
            "SMCP_MODELS_REGISTRY": str(self.registry),
            "SMCP_MODELS_DIR": str(self.models),
            "SMCP_CONDA_EXE": str(self.conda),
        })
        self.environment.start()

    def tearDown(self):
        self.environment.stop()
        self.temp.cleanup()

    def test_registry_exposes_all_models_and_parameters(self):
        models, errors = smcp.load_registry()
        self.assertEqual(errors, [])
        self.assertEqual(set(models), {"score-denoise", "straightPCF", "pointcleannet"})
        self.assertIn("cluster_size", models["score-denoise"]["configuration"])
        self.assertIn("niters", models["straightPCF"]["configuration"])
        self.assertIn("iterations", models["pointcleannet"]["configuration"])

    def test_configuration_rejects_unknown_and_out_of_range_values(self):
        models, _ = smcp.load_registry()
        schema = models["pointcleannet"]["configuration_schema"]
        with self.assertRaisesRegex(smcp.RegistryError, "unknown"):
            smcp.validate_configuration({"mystery": 1}, schema, "configuration")
        with self.assertRaisesRegex(smcp.RegistryError, "less than 1"):
            smcp.validate_configuration({"overlap": 1.0}, schema, "configuration")

    def test_output_suffix_is_added_once(self):
        source = self.root / "piece.xyz"
        expected = self.root / "piece_score_denoise.xyz"
        self.assertEqual(smcp.output_path(str(self.root), source, "score_denoise"), expected)
        self.assertEqual(smcp.output_path(str(expected), source, "score_denoise"), expected)

    def test_inference_passes_json_configuration_as_runner_arguments(self):
        input_path = self.root / "input.xyz"
        input_path.write_text("0 0 0\n", encoding="utf-8")
        config_path = self.root / "config.json"
        config_path.write_text('{"niters": 4, "cluster_size": 5000}', encoding="utf-8")
        output_path = self.root / "output_straightPCF.xyz"
        completed = subprocess.CompletedProcess([], 0)
        with mock.patch.object(sys, "argv", [
            "smcp", "--model", "straightPCF", "--input", str(input_path),
            "--output", str(output_path), "--config-json", str(config_path),
        ]), mock.patch.object(smcp.subprocess, "run", return_value=completed) as run:
            self.assertEqual(smcp.main(), 0)
        command = run.call_args.args[0]
        self.assertIn("--niters", command)
        self.assertEqual(command[command.index("--niters") + 1], "4")
        self.assertEqual(command[command.index("--cluster-size") + 1], "5000")

    def test_disabled_model_does_not_require_artifacts(self):
        document = json.loads(self.registry.read_text(encoding="utf-8"))
        disabled = dict(document["models"][0])
        disabled.update({"id": "disabled-model", "directory": "missing", "enabled": False})
        document["models"].append(disabled)
        self.registry.write_text(json.dumps(document), encoding="utf-8")
        models, errors = smcp.load_registry()
        self.assertEqual(errors, [])
        self.assertNotIn("disabled-model", models)

    def find_processes(self, marker):
        pids = []
        for entry in Path("/proc").iterdir():
            if not entry.name.isdigit():
                continue
            try:
                arguments = (entry / "cmdline").read_bytes().split(b"\0")
                state = (entry / "stat").read_text(encoding="ascii").rsplit(")", 1)[1].split()[0]
            except (OSError, IndexError):
                continue
            if marker.encode() in arguments and state != "Z":
                pids.append(int(entry.name))
        return pids

    def wait_until(self, condition, timeout=15.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if condition():
                return True
            time.sleep(0.1)
        return condition()

    def test_supervisor_returns_the_command_exit_code(self):
        read_fd, write_fd = os.pipe()
        try:
            supervisor = subprocess.Popen(
                [sys.executable, str(SCRIPT), smcp.SUPERVISOR_FLAG, str(read_fd), "-", "--", "sh", "-c", "exit 3"],
                pass_fds=(read_fd,), start_new_session=True,
            )
            os.close(read_fd)
            self.assertEqual(supervisor.wait(timeout=15), 3)
        finally:
            os.close(write_fd)

    def test_abandoned_inference_job_is_terminated(self):
        # A fake conda that behaves like a long inference, identified by a unique sleep duration.
        marker = "1234.25"
        self.conda.write_text(f"#!/bin/sh\nexec sleep {marker}\n", encoding="ascii")
        self.conda.chmod(0o755)
        input_path = self.root / "input.xyz"
        input_path.write_text("0 0 0\n", encoding="utf-8")
        job_id = str(uuid.uuid4())

        launcher = subprocess.Popen([
            sys.executable, str(SCRIPT), "--model", "score-denoise", "--input", str(input_path),
            "--output", str(self.root / "output_score_denoise.xyz"), "--job-id", job_id,
        ], start_new_session=True)
        try:
            self.assertTrue(self.wait_until(lambda: self.find_processes(marker)), "the fake inference did not start")
            # Windows terminating wsl.exe kills the launcher without letting it clean up.
            launcher.kill()
            launcher.wait(timeout=5)
            self.assertTrue(self.wait_until(lambda: not self.find_processes(marker)),
                            "the inference kept running after its launcher was killed")
            self.assertTrue(self.wait_until(lambda: not smcp.job_file(job_id).exists()))
        finally:
            for pid in self.find_processes(marker):
                os.kill(pid, signal.SIGKILL)

    def test_cancel_job_stops_a_running_inference(self):
        marker = "1234.75"
        self.conda.write_text(f"#!/bin/sh\nexec sleep {marker}\n", encoding="ascii")
        self.conda.chmod(0o755)
        input_path = self.root / "input.xyz"
        input_path.write_text("0 0 0\n", encoding="utf-8")
        job_id = str(uuid.uuid4())

        launcher = subprocess.Popen([
            sys.executable, str(SCRIPT), "--model", "score-denoise", "--input", str(input_path),
            "--output", str(self.root / "output_score_denoise.xyz"), "--job-id", job_id,
        ])
        try:
            self.assertTrue(self.wait_until(lambda: self.find_processes(marker) and smcp.job_file(job_id).exists()))
            cancel = subprocess.run([sys.executable, str(SCRIPT), "--cancel-job", job_id], check=False)
            self.assertEqual(cancel.returncode, 0)
            self.assertNotEqual(launcher.wait(timeout=15), 0)
            self.assertFalse(self.find_processes(marker))
        finally:
            if launcher.poll() is None:
                launcher.kill()
            for pid in self.find_processes(marker):
                os.kill(pid, signal.SIGKILL)

    def test_cancel_job_terminates_the_registered_process_group(self):
        pid_path = self.root / "test-job.pid"
        process = subprocess.Popen(["sleep", "30"], start_new_session=True)
        pid_path.write_text(str(process.pid), encoding="ascii")
        try:
            with mock.patch.object(smcp, "job_file", return_value=pid_path):
                self.assertEqual(smcp.cancel_job("12345678-1234-1234-1234-123456789abc"), 0)
            self.assertEqual(process.wait(timeout=5), -signal.SIGTERM)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)


if __name__ == "__main__":
    unittest.main()
