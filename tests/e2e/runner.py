#!/usr/bin/env python3
"""Small dependency-free flow runner for Hplayer end-to-end cases."""

from __future__ import annotations

import json
import os
import shlex
import subprocess
import time
import urllib.error
import urllib.request
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


class TestFailure(RuntimeError):
    pass


def resolve_value(value: Any, variables: dict[str, Any]) -> Any:
    if isinstance(value, str):
        pattern = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")
        if value.startswith("$" + "{") and value.endswith("}") and pattern.fullmatch(value):
            return variables.get(value[2:-1], value)
        return pattern.sub(lambda match: str(variables.get(match.group(1), match.group(0))), value)
    if isinstance(value, dict):
        return {key: resolve_value(item, variables) for key, item in value.items()}
    if isinstance(value, list):
        return [resolve_value(item, variables) for item in value]
    return value


def value_at(document: Any, path: str) -> Any:
    current = document
    for part in path.split(".") if path else []:
        if isinstance(current, dict) and part in current:
            current = current[part]
        elif isinstance(current, list) and part.isdigit() and int(part) < len(current):
            current = current[int(part)]
        else:
            raise TestFailure(f"Path not found: {path}")
    return current


class HttpAdapter:
    def __init__(
        self,
        base_url: str,
        timeout: float = 15.0,
        default_headers: dict[str, str] | None = None,
    ):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.default_headers = default_headers or {}

    def login(
        self,
        username: str,
        password: str,
        path: str = "/api/v1/auth/login",
        login_type: str = "local",
        timeout: float = 15.0,
    ) -> dict[str, Any]:
        request = urllib.request.Request(
            self.base_url.rstrip("/") + path,
            data=json.dumps(
                {"username": username, "password": password, "loginType": login_type}
            ).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                raw = response.read().decode("utf-8", errors="replace")
                status = response.status
        except urllib.error.HTTPError as error:
            raw = error.read().decode("utf-8", errors="replace")
            status = error.code
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            raise TestFailure(f"Login failed: {error}") from error
        try:
            body = json.loads(raw) if raw else None
        except json.JSONDecodeError:
            body = raw
        if status < 200 or status >= 300 or not isinstance(body, dict):
            raise TestFailure(f"Login returned HTTP {status}: {raw[:500]}")
        data = body.get("data") if isinstance(body.get("data"), dict) else {}
        token = data.get("token")
        if not isinstance(token, str) or not token:
            raise TestFailure(f"Login response did not contain a token: {raw[:500]}")
        self.default_headers["Authorization"] = f"Bearer {token}"
        return {"status": status, "body": body, "url": request.full_url}

    def request(self, step: dict[str, Any], variables: dict[str, Any]) -> dict[str, Any]:
        method = str(step.get("method", "POST")).upper()
        path = str(resolve_value(step.get("path", "/"), variables))
        url = path if path.startswith(("http://", "https://")) else self.base_url + path
        body = resolve_value(step.get("body"), variables)
        headers = dict(self.default_headers)
        headers.update(
            {
                str(key): str(resolve_value(value, variables))
                for key, value in step.get("headers", {}).items()
            }
        )
        data = None
        if body is not None:
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
            headers.setdefault("Content-Type", "application/json")
        request = urllib.request.Request(url, data=data, headers=headers, method=method)
        started = time.monotonic()
        try:
            with urllib.request.urlopen(
                request, timeout=float(step.get("timeout", self.timeout))
            ) as response:
                raw = response.read().decode("utf-8", errors="replace")
                status = response.status
        except urllib.error.HTTPError as error:
            raw = error.read().decode("utf-8", errors="replace")
            status = error.code
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            raise TestFailure(f"HTTP {method} {url} failed: {error}") from error
        try:
            body_value = json.loads(raw) if raw else None
        except json.JSONDecodeError:
            body_value = raw
        result = {
            "status": status,
            "body": body_value,
            "raw": raw,
            "elapsed_ms": round((time.monotonic() - started) * 1000, 1),
            "url": url,
        }
        expected = step.get("expect_status")
        allowed = step.get("expect_status_any")
        allowed_statuses = [int(item) for item in allowed] if isinstance(allowed, list) else None
        if expected is not None and status != int(expected):
            raise TestFailure(
                f"HTTP {method} {url} returned {status}, expected {expected}: {raw[:500]}"
            )
        if allowed_statuses is not None and status not in allowed_statuses:
            raise TestFailure(
                f"HTTP {method} {url} returned {status}, expected one of {allowed_statuses}: {raw[:500]}"
            )
        if status == 403 and step.get("allow_blocked"):
            result["outcome"] = "blocked"
        return result


class AdbAdapter:
    def __init__(self, adb: str = "adb", serial: str | None = None, timeout: float = 30.0):
        self.adb = adb
        self.serial = serial
        self.timeout = timeout

    def run(
        self, step: dict[str, Any], variables: dict[str, Any], artifact_dir: Path
    ) -> dict[str, Any]:
        command = step.get("command")
        if isinstance(command, str):
            command = shlex.split(command, posix=os.name != "nt")
        if not isinstance(command, list) or not command:
            raise TestFailure("ADB step requires a non-empty command list or string")
        args = [self.adb]
        if self.serial:
            args += ["-s", self.serial]
        args += [str(resolve_value(item, variables)) for item in command]
        started = time.monotonic()
        completed = subprocess.run(
            args,
            capture_output=True,
            text=True,
            timeout=float(step.get("timeout", self.timeout)),
        )
        result = {
            "command": args,
            "returncode": completed.returncode,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
            "elapsed_ms": round((time.monotonic() - started) * 1000, 1),
        }
        if step.get("save_stdout"):
            target = artifact_dir / str(step["save_stdout"])
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(completed.stdout, encoding="utf-8")
        expected = int(step.get("expect_returncode", 0))
        if expected != completed.returncode:
            raise TestFailure(
                f"ADB command failed ({completed.returncode}): {' '.join(args)}\n"
                f"{completed.stderr}"
            )
        return result

    def capture(
        self, step: dict[str, Any], variables: dict[str, Any], artifact_dir: Path
    ) -> dict[str, Any]:
        command = step.get("command")
        if isinstance(command, str):
            command = shlex.split(command, posix=os.name != "nt")
        if not isinstance(command, list) or not command:
            raise TestFailure("ADB capture step requires a non-empty command")
        args = [self.adb]
        if self.serial:
            args += ["-s", self.serial]
        args += [str(resolve_value(item, variables)) for item in command]
        completed = subprocess.run(
            args,
            capture_output=True,
            timeout=float(step.get("timeout", self.timeout)),
        )
        target = artifact_dir / str(step.get("save_file", "adb-capture.bin"))
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(completed.stdout)
        if completed.returncode != int(step.get("expect_returncode", 0)):
            stderr = completed.stderr.decode("utf-8", errors="replace")
            raise TestFailure(f"ADB capture failed: {' '.join(args)}\n{stderr}")
        return {
            "command": args,
            "returncode": completed.returncode,
            "file": str(target),
            "bytes": len(completed.stdout),
        }


@dataclass
class CaseResult:
    case_id: str
    name: str
    status: str
    duration_ms: float
    steps: list[dict[str, Any]] = field(default_factory=list)
    error: str | None = None


class FlowRunner:
    def __init__(
        self,
        *,
        base_url: str,
        adb: str,
        serial: str | None,
        artifact_dir: Path,
        default_headers: dict[str, str] | None = None,
        global_variables: dict[str, Any] | None = None,
    ):
        self.http = HttpAdapter(base_url, default_headers=default_headers)
        self.adb = AdbAdapter(adb, serial)
        self.artifact_dir = artifact_dir
        self.global_variables = global_variables or {}

    def execute_step(self, step: dict[str, Any], variables: dict[str, Any]) -> dict[str, Any]:
        step_type = step.get("type")
        if step_type == "set":
            variables[str(step["name"])] = resolve_value(step.get("value"), variables)
            return {"name": step["name"], "value": variables[str(step["name"])]}
        if step_type == "wait":
            time.sleep(float(step.get("seconds", 1)))
            return {"seconds": step.get("seconds", 1)}
        if step_type == "note":
            return {"message": str(step.get("message", ""))}
        if step_type == "http":
            result = self.http.request(step, variables)
            if step.get("save_as"):
                variables[str(step["save_as"])] = result
            return result
        if step_type == "login":
            result = self.http.login(
                username=str(resolve_value(step.get("username", ""), variables)),
                password=str(resolve_value(step.get("password", ""), variables)),
                path=str(step.get("path", "/api/v1/auth/login")),
                login_type=str(step.get("login_type", "local")),
                timeout=float(step.get("timeout", 15)),
            )
            if step.get("save_as"):
                variables[str(step["save_as"])] = result
            return result
        if step_type == "adb":
            result = self.adb.run(step, variables, self.artifact_dir)
            if step.get("save_as"):
                variables[str(step["save_as"])] = result
            return result
        if step_type == "adb_capture":
            result = self.adb.capture(step, variables, self.artifact_dir)
            if step.get("save_as"):
                variables[str(step["save_as"])] = result
            return result
        if step_type == "assert":
            document = variables if step.get("from") == "vars" else variables.get("last", {})
            path = str(step.get("path", ""))
            try:
                actual = value_at(document, path)
                exists = True
            except TestFailure:
                actual = None
                exists = False
            if "exists" in step and bool(step["exists"]) != exists:
                raise TestFailure(f"Assertion failed: {path} exists={exists}, expected {step['exists']}")
            if not exists:
                return {"path": path, "exists": False}
            if "equals" in step and actual != resolve_value(step["equals"], variables):
                raise TestFailure(
                    f"Assertion failed: {step['path']} = {actual!r}, "
                    f"expected {step['equals']!r}"
                )
            if "contains" in step and str(resolve_value(step["contains"], variables)) not in str(actual):
                raise TestFailure(
                    f"Assertion failed: {step['path']} does not contain {step['contains']!r}"
                )
            if "not_equals" in step and actual == resolve_value(step["not_equals"], variables):
                raise TestFailure(f"Assertion failed: {path} unexpectedly equals {actual!r}")
            if "matches" in step and re.search(str(resolve_value(step["matches"], variables)), str(actual)) is None:
                raise TestFailure(f"Assertion failed: {path} does not match {step['matches']!r}")
            return {"path": step["path"], "actual": actual}
        raise TestFailure(f"Unsupported step type: {step_type}")

    def run_case(self, case: dict[str, Any], include_destructive: bool = False) -> CaseResult:
        started = time.monotonic()
        case_id = str(case.get("id", "unnamed"))
        name = str(case.get("name", case_id))
        if case.get("enabled", True) is False:
            return CaseResult(case_id, name, "skipped", 0.0)
        if case.get("destructive", False) and not include_destructive:
            return CaseResult(case_id, name, "skipped", 0.0, error="destructive test excluded")
        variables = dict(self.global_variables)
        variables.update({str(key): value for key, value in case.get("variables", {}).items()})
        steps: list[dict[str, Any]] = []
        failure: Exception | None = None
        try:
            all_steps = list(case.get("setup", [])) + list(case.get("steps", []))
            blocked = False
            for index, step in enumerate(all_steps, start=1):
                attempts = int(step.get("retries", 0)) + 1
                for attempt in range(1, attempts + 1):
                    try:
                        result = self.execute_step(step, variables)
                        variables["last"] = result
                        steps.append(
                            {
                                "index": index,
                                "type": step.get("type"),
                                "attempt": attempt,
                                "status": "blocked" if result.get("outcome") == "blocked" else "passed",
                                "result": result,
                            }
                        )
                        failure = None
                        if result.get("outcome") == "blocked":
                            blocked = True
                        break
                    except Exception as error:  # noqa: BLE001
                        failure = error
                        if attempt >= attempts:
                            raise
                        time.sleep(float(step.get("retry_delay", 0.5)))
        except Exception as error:  # noqa: BLE001 - preserve failure in report
            failure = error
            steps.append(
                {
                    "index": len(steps) + 1,
                    "status": "failed",
                    "error": str(error),
                }
            )
        cleanup_errors = []
        for cleanup_step in case.get("cleanup", []):
            try:
                cleanup_result = self.execute_step(cleanup_step, variables)
                steps.append(
                    {
                        "index": "cleanup",
                        "type": cleanup_step.get("type"),
                        "status": "passed",
                        "result": cleanup_result,
                    }
                )
            except Exception as error:  # noqa: BLE001
                cleanup_errors.append(str(error))
                steps.append(
                    {
                        "index": "cleanup",
                        "type": cleanup_step.get("type"),
                        "status": "failed",
                        "error": str(error),
                    }
                )
        if failure is None and cleanup_errors:
            failure = TestFailure("Cleanup failed: " + "; ".join(cleanup_errors))
        return CaseResult(
            case_id,
            name,
            "failed" if failure else ("blocked" if blocked else "passed"),
            round((time.monotonic() - started) * 1000, 1),
            steps,
            str(failure) if failure else None,
        )


def load_cases(path: Path) -> list[dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(payload, dict):
        return [payload]
    if isinstance(payload, list):
        return payload
    raise ValueError(f"Case file must contain an object or array: {path}")
