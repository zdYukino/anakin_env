"""Launcher checks use actual local Node/ROS dependencies and isolated HTTP ports."""

import json
import os
import signal
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from contextlib import contextmanager
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "em_run.sh"


class LauncherTest(unittest.TestCase):
    def run_launcher(self, *args):
        return subprocess.run(
            [str(LAUNCHER), *args],
            cwd="/tmp",
            capture_output=True,
            text=True,
            timeout=60,
        )

    def test_help_and_invalid_arguments(self):
        result = self.run_launcher("--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("--web", result.stdout)
        for args in [
            ("--port", "65536"),
            ("--port",),
            ("--domain", "233"),
            ("--unknown",),
        ]:
            result = self.run_launcher(*args)
            self.assertEqual(result.returncode, 2)
            self.assertIn("Startup failed", result.stderr)

    @contextmanager
    def service(self, *args, env=None):
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        with tempfile.TemporaryFile(mode="w+") as log:
            process = subprocess.Popen(
                [str(LAUNCHER), "--no-install", "--port", str(port), *args],
                cwd="/tmp",
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            try:
                for _ in range(150):
                    if process.poll() is not None:
                        log.seek(0)
                        self.fail(log.read())
                    try:
                        with urllib.request.urlopen(
                            f"http://127.0.0.1:{port}/", timeout=1
                        ) as response:
                            body = response.read().decode()
                        break
                    except (OSError, urllib.error.URLError):
                        time.sleep(0.1)
                else:
                    log.seek(0)
                    self.fail("Startup timed out: " + log.read())
                self.assertIn("Aerospace", body)
                yield port
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
            self.assertIsNotNone(process.returncode)

    def test_web_only_and_cached_build_without_python_or_ros(self):
        env = os.environ.copy()
        env.update(
            TELEMETRY_PYTHON="/missing/python",
            TELEMETRY_ROS_SETUP="/missing/setup.bash",
        )
        with self.service("--web", env=env):
            pass
        before = (ROOT / "dist/index.html").stat().st_mtime_ns
        result = self.run_launcher("--web", "--prepare", "--no-install")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Reusing current build", result.stdout)
        self.assertEqual((ROOT / "dist/index.html").stat().st_mtime_ns, before)

    def test_gateway_serves_frontend_and_read_only_health(self):
        with self.service("--domain", "195") as port:
            with urllib.request.urlopen(
                f"http://127.0.0.1:{port}/api/health", timeout=3
            ) as response:
                health = json.load(response)
            self.assertTrue(health["ready"], health)
            self.assertTrue(health["read_only"])
            self.assertEqual(health["domain_id"], "195")
            self.assertEqual(health["adapter"], "ros2")


if __name__ == "__main__":
    unittest.main()
