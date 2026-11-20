from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from runner import FlowRunner, TestFailure, resolve_value


class RunnerTests(unittest.TestCase):
    def test_resolves_nested_variables(self) -> None:
        placeholder = "$" + "{item}"
        self.assertEqual(
            resolve_value({"id": placeholder, "items": [placeholder]}, {"item": 17}),
            {"id": 17, "items": [17]},
        )

    def test_assertion_and_cleanup_run_after_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            runner = FlowRunner(
                base_url="http://127.0.0.1",
                adb="adb",
                serial=None,
                artifact_dir=Path(temporary),
            )
            case = {
                "id": "cleanup-on-failure",
                "steps": [{"type": "assert", "from": "vars", "path": "missing", "exists": True}],
                "cleanup": [{"type": "set", "name": "cleaned", "value": True}],
            }
            result = runner.run_case(case)
            self.assertEqual(result.status, "failed")
            self.assertEqual(result.steps[-1]["status"], "passed")

    def test_retries_then_passes_and_records_attempt(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            runner = FlowRunner(
                base_url="http://127.0.0.1",
                adb="adb",
                serial=None,
                artifact_dir=Path(temporary),
            )
            step = {"type": "http", "path": "/status", "retries": 1, "retry_delay": 0}
            with patch.object(
                runner.http,
                "request",
                side_effect=[TestFailure("temporary"), {"status": 200}],
            ):
                result = runner.run_case({"id": "retry", "steps": [step]})
            self.assertEqual(result.status, "passed")
            self.assertEqual(result.steps[0]["attempt"], 2)

    def test_http_adapter_uses_default_header(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            runner = FlowRunner(
                base_url="http://127.0.0.1",
                adb="adb",
                serial=None,
                artifact_dir=Path(temporary),
                default_headers={"Authorization": "Bearer test"},
            )
            self.assertEqual(runner.http.default_headers["Authorization"], "Bearer test")


if __name__ == "__main__":
    unittest.main()
