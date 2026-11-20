#!/usr/bin/env python3
"""Discover and validate resources required by real Hplayer flows."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
MANIFEST = Path(__file__).with_name("resource_manifest.json")
DEFAULT_ARTIFACTS = ROOT / "tests" / "e2e" / ".artifacts"


def request_json(url: str, *, headers: dict[str, str] | None = None, method: str = "GET", body: Any = None) -> dict[str, Any]:
    data = None
    request_headers = dict(headers or {})
    if body is not None:
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        request_headers.setdefault("Content-Type", "application/json")
    request = urllib.request.Request(url, data=data, headers=request_headers, method=method)
    with urllib.request.urlopen(request, timeout=12) as response:
        raw = response.read().decode("utf-8", errors="replace")
        payload = json.loads(raw) if raw else {}
        return {"status": response.status, "body": payload, "url": url}


def safe_request(url: str, **kwargs: Any) -> dict[str, Any]:
    try:
        return request_json(url, **kwargs)
    except urllib.error.HTTPError as error:
        raw = error.read().decode("utf-8", errors="replace")
        try:
            body = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            body = {"raw": raw[:500]}
        return {"status": error.code, "body": body, "url": url, "error": f"HTTP {error.code}"}
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        return {"status": None, "body": {}, "url": url, "error": str(error)}


def adb_devices(adb: str) -> tuple[list[str], str | None]:
    try:
        completed = subprocess.run([adb, "devices"], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError) as error:
        return [], str(error)
    if completed.returncode != 0:
        return [], completed.stderr.strip() or f"adb exited {completed.returncode}"
    devices = []
    for line in completed.stdout.splitlines():
        parts = line.strip().split()
        if len(parts) >= 2 and parts[1] == "device":
            devices.append(parts[0])
    return devices, None


def adb_shell(adb: str, serial: str, *command: str) -> str | None:
    try:
        completed = subprocess.run(
            [adb, "-s", serial, "shell", *command],
            capture_output=True,
            text=True,
            timeout=12,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return completed.stdout.strip() if completed.returncode == 0 else None


def probe_endpoint(base_url: str, path: str, headers: dict[str, str], method: str = "GET") -> dict[str, Any]:
    result = safe_request(
        base_url.rstrip("/") + path,
        headers=headers,
        method=method,
        body={} if method.upper() != "GET" else None,
    )
    status = result.get("status")
    if status is None:
        result["outcome"] = "failed"
    elif status == 403:
        result["outcome"] = "blocked"
    elif isinstance(status, int) and 200 <= status < 300:
        result["outcome"] = "passed"
    else:
        result["outcome"] = "failed"
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepare real-device Hplayer test resources")
    parser.add_argument("--serial", help="player ADB serial; defaults to the only online device")
    parser.add_argument("--pad-serial", help="Pad ADB serial; optional")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--player-url", default="")
    parser.add_argument("--server-url", default="http://192.168.2.105:9898")
    parser.add_argument("--username", default="admin")
    parser.add_argument("--password", default=os.environ.get("HPLAYER_PASSWORD", "admin*"))
    parser.add_argument("--artifacts", type=Path, default=DEFAULT_ARTIFACTS)
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    devices, adb_error = adb_devices(args.adb)
    serial = args.serial or (devices[0] if len(devices) == 1 else None)
    player_url = args.player_url.strip()
    if not player_url and serial and ":" in serial:
        player_url = f"http://{serial.rsplit(':', 1)[0]}:8080"

    inventory: dict[str, Any] = {
        "schema_version": 1,
        "generated_at": datetime.now().astimezone().isoformat(timespec="seconds"),
        "inputs": {"adb": args.adb, "serial": serial, "player_url": player_url, "server_url": args.server_url},
        "devices": {"online": devices, "adb_error": adb_error},
        "checks": {},
        "selected_song": None,
        "summary": {"passed": 0, "failed": 0, "blocked": 0},
    }
    checks = inventory["checks"]
    checks["adb"] = {
        "outcome": "passed" if serial else ("failed" if adb_error else "blocked"),
        "online_devices": devices,
        "serial": serial,
        "error": adb_error,
    }

    player_headers: dict[str, str] = {}
    if player_url:
        login = safe_request(
            player_url.rstrip("/") + "/api/v1/auth/login",
            method="POST",
            body={"username": args.username, "password": args.password, "loginType": "local"},
        )
        body = login.get("body") if isinstance(login.get("body"), dict) else {}
        token = (body.get("data") or {}).get("token") if isinstance(body.get("data"), dict) else None
        if isinstance(token, str) and token:
            player_headers["Authorization"] = f"Bearer {token}"
            checks["player_login"] = {"outcome": "passed", "status": login.get("status")}
        else:
            checks["player_login"] = {"outcome": "failed", "status": login.get("status"), "error": login.get("error", "token missing")}
        for path in manifest["player"]["required_endpoints"] + manifest["player"]["read_only_endpoints"]:
            checks[f"player:{path}"] = probe_endpoint(player_url, path, player_headers)
        for capability, config in manifest["hardware"].items():
            checks[f"hardware:{capability}"] = probe_endpoint(
                player_url,
                config["probe"],
                player_headers,
                str(config.get("method", "GET")),
            )
    else:
        checks["player_login"] = {"outcome": "blocked", "error": "No player URL or IP ADB device"}

    server_headers: dict[str, str] = {}
    for path in manifest["server"]["required_endpoints"]:
        result = probe_endpoint(args.server_url, path, server_headers)
        checks[f"server:{path}"] = result
        if path == "/api/v1/terminals/pad-list" and result.get("outcome") == "passed":
            data = ((result.get("body") or {}).get("data") or [])
            player_host = ""
            if player_url:
                player_host = player_url.split("//", 1)[-1].split(":", 1)[0]
            terminals = [item for item in data if isinstance(item, dict)] if isinstance(data, list) else []
            terminal = next(
                (item for item in terminals if item.get("terminalIp") == player_host),
                terminals[0] if len(terminals) == 1 else None,
            )
            room_id = terminal.get("roomId") if isinstance(terminal, dict) else None
            inventory["selected_terminal"] = {
                "id": terminal.get("id") if isinstance(terminal, dict) else None,
                "terminalIp": terminal.get("terminalIp") if isinstance(terminal, dict) else None,
                "roomId": room_id,
                "onlineStatus": terminal.get("onlineStatus") if isinstance(terminal, dict) else None,
            }
            checks["room_binding"] = {
                "outcome": "passed" if room_id else "blocked",
                "room_id": room_id,
                "terminal": inventory["selected_terminal"],
                "error": None if room_id else "Selected player has no roomId binding",
            }
        if path.startswith("/api/v1/songdb/songs") and result.get("outcome") == "passed":
            data = ((result.get("body") or {}).get("data") or {})
            items = data.get("items") if isinstance(data, dict) else []
            if isinstance(items, list) and items:
                candidate = items[0]
                required = manifest["server"]["song_fixture"]["required_fields"]
                if all(candidate.get(key) is not None for key in required):
                    inventory["selected_song"] = {key: candidate.get(key) for key in required}
    song_ready = inventory["selected_song"] is not None
    checks["media_fixture"] = {
        "outcome": "passed" if song_ready else "blocked",
        "song": inventory["selected_song"],
    }

    apk_paths = manifest["pad"]["apk_paths"]
    for name, raw_path in apk_paths.items():
        path = Path(raw_path)
        checks[f"pad_apk:{name}"] = {"outcome": "passed" if path.is_file() else "blocked", "path": str(path), "bytes": path.stat().st_size if path.is_file() else 0}
    pad_serial = args.pad_serial
    if pad_serial:
        packages = adb_shell(args.adb, pad_serial, "pm", "list", "packages") or ""
        for package in manifest["pad"]["packages"]:
            checks[f"pad_package:{package}"] = {"outcome": "passed" if f"package:{package}" in packages else "blocked", "serial": pad_serial}
    else:
        checks["pad_device"] = {"outcome": "blocked", "error": "Pad ADB serial not provided"}

    for check in checks.values():
        outcome = check.get("outcome")
        if outcome in inventory["summary"]:
            inventory["summary"][outcome] += 1
    args.artifacts.mkdir(parents=True, exist_ok=True)
    output = args.artifacts / "resource-inventory.json"
    output.write_text(json.dumps(inventory, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(output), "summary": inventory["summary"], "selected_song": inventory["selected_song"]}, ensure_ascii=False))
    if inventory["summary"]["failed"]:
        return 1
    if inventory["summary"]["blocked"]:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
