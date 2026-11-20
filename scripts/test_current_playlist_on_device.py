"""Opt-in playback regression on a specified player; restores its original media."""
import argparse
import json
from pathlib import Path
import subprocess
import time
import urllib.error
import urllib.request

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--device", required=True)
p.add_argument("--output", required=True, type=Path)
args = p.parse_args()
base = f"http://{args.device}:8080"
token = None
report = {"device": args.device, "checks": []}


def request(path, body=None):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(base + path,
                                 json.dumps(body).encode() if body is not None else None,
                                 headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        result = json.load(r)
    assert result.get("ok"), result
    return result["data"]


def status():
    return request("/api/v1/video/status?layerId=1")


def wait_path(path):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        value = status()
        if value["path"] == path and value["state"] == "playing" and value["duration"] > 0:
            return value
        time.sleep(0.3)
    raise AssertionError(value)


password = subprocess.check_output(
    ["adb", "-s", args.device + ":5555", "shell", "getprop", "persist.hsvj.web_password"],
    text=True).strip() or "admin*"
token = request("/api/v1/auth/login", {
    "username": "admin", "password": password, "loginType": "local"})["token"]
original = status()
report["original"] = original
try:
    playlists = request("/api/v1/playlists")
    chosen = next(v for v in playlists if v["targetLayerId"] == 1 and not v["isDefault"] and v["count"] >= 2)
    playlist_id = chosen["id"]
    initial = request("/api/v1/video/play", {"playlistId": playlist_id, "index": 0})
    first = wait_path(initial["uri"])
    request("/api/v1/video/seek", {"layerId": 1, "position": min(5, first["duration"] / 4)})
    for _ in range(3):
        request("/api/v1/video/pause", {"layerId": 1})
        paused = status()
        assert paused["state"] == "paused" and paused["playlistId"] == playlist_id, paused
        resumed = request("/api/v1/video/play", {"layerId": 1})
        current = status()
        assert resumed["playlistId"] == playlist_id, resumed
        assert current["playlistId"] == playlist_id and current["path"] == first["path"], current
        assert current["state"] == "playing", current
        assert abs(current["current_position"] - paused["current_position"]) < 2, (paused, current)
        report["checks"].append({"action": "pause/play", "paused": paused, "after": current})
    for action in ["next", "previous", "next", "previous"]:
        result = request("/api/v1/playback/actions/" + action, {})
        assert result["playlistId"] == playlist_id, result
        current = wait_path(result["uri"])
        assert current["playlistId"] == playlist_id, current
        report["checks"].append({"action": action, "result": result, "after": current})
    default = next(v for v in playlists if v["targetLayerId"] == 1 and v["isDefault"])
    result = request("/api/v1/video/play", {"playlistId": default["id"], "index": 0})
    current = wait_path(result["uri"])
    assert current["playlistId"] == default["id"], current
    report["checks"].append({"action": "explicit default switch", "after": current})
    materials = request("/api/v1/materials?type=video")
    legacy = next(v for v in materials if "%E7%8E%8B" in v["path"])
    assert legacy["name"] == "王杰 - 英雄泪.mp4", legacy
    report["materials"] = materials
    report["ok"] = True
finally:
    if original.get("path"):
        request("/api/v1/video/play", {"path": original["path"], "layerId": 1})
        wait_path(original["path"])
        request("/api/v1/video/seek", {"position": original["current_position"], "layerId": 1})
        if original["state"] == "paused":
            request("/api/v1/video/pause", {"layerId": 1})
        elif original["state"] == "stopped":
            request("/api/v1/video/stop", {"layerId": 1})
        report["restored"] = status()
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print("PASS: device pause/play preserves current URI and position, next/previous preserves current playlist, explicit list switch and cloud material name")
